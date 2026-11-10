#include "navigation.hpp"
#include "platform/path.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace liltview
{
namespace
{
unsigned char folded(unsigned char value)
{
    return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}

bool digit(unsigned char value)
{
    return value >= '0' && value <= '9';
}

std::string normalized_extension(std::string value)
{
    if (!value.empty() && value.front() == '.')
        value.erase(value.begin());
    for (auto &character : value)
        character = static_cast<char>(folded(static_cast<unsigned char>(character)));
    return value;
}

// Compare numeric runs by their significant digits, without narrowing to an integer.
// Equal numbers and ASCII case differences use the original full filename as final tie-break.
bool natural_less(const std::filesystem::path &left, const std::filesystem::path &right)
{
    const auto a  = platform::path_bytes(left.filename());
    const auto b  = platform::path_bytes(right.filename());
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size())
    {
        const auto ac = static_cast<unsigned char>(a[i]), bc = static_cast<unsigned char>(b[j]);
        if (digit(ac) && digit(bc))
        {
            const auto ai = i, bj = j;
            while (i < a.size() && digit(static_cast<unsigned char>(a[i])))
                ++i;
            while (j < b.size() && digit(static_cast<unsigned char>(b[j])))
                ++j;
            auto az = ai, bz = bj;
            while (az < i && a[az] == '0')
                ++az;
            while (bz < j && b[bz] == '0')
                ++bz;
            const auto an = i - az, bn = j - bz;
            if (an != bn)
                return an < bn;
            const auto order = a.compare(az, an, b, bz, bn);
            if (order != 0)
                return order < 0;
        }
        else
        {
            if (folded(ac) != folded(bc))
                return folded(ac) < folded(bc);
            ++i;
            ++j;
        }
    }
    if (i != a.size() || j != b.size())
        return i == a.size();
    return a < b;
}

bool add_size(std::size_t &bytes, const std::filesystem::path &path, const DirectoryNavigationOptions &options)
{
    const auto count = path.native().size();
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(std::filesystem::path::value_type))
        return false;
    const auto size = count * sizeof(std::filesystem::path::value_type);
    if (bytes > options.max_path_bytes || size > options.max_path_bytes - bytes)
        return false;
    bytes += size;
    return true;
}

DirectoryScanResult scan_directory(const std::filesystem::path &directory, const std::vector<std::string> &extensions,
                                   const DirectoryNavigationOptions &options, const DirectoryNavigation::Cancelled &cancelled)
{
    DirectoryScanResult result;
    std::filesystem::directory_iterator iterator(directory, result.error), end;
    if (result.error)
        return result;
    std::size_t bytes = 0;
    while (iterator != end)
    {
        if (cancelled())
            return {};
        const auto &entry    = *iterator;
        const auto extension = normalized_extension(platform::path_bytes(entry.path().extension()));
        std::error_code error;
        if (std::binary_search(extensions.begin(), extensions.end(), extension) && entry.is_regular_file(error))
        {
            const auto path = entry.path();
            if (result.entries.size() >= options.max_entries || !add_size(bytes, path, options))
            {
                result.entries.clear();
                result.error = std::make_error_code(std::errc::value_too_large);
                return result;
            }
            result.entries.push_back(path);
        }
        // A broken link or vanished entry is skipped; failure to enumerate the directory is not.
        iterator.increment(result.error);
        if (result.error)
        {
            result.entries.clear();
            return result;
        }
    }
    return result;
}

struct ScanCancelled
{
};
} // namespace

struct DirectoryNavigation::State
{
    struct Task
    {
        std::filesystem::path selected;
        std::uint64_t generation = 0;
    };
    struct Result
    {
        ScanResult scan;
        std::uint64_t generation = 0;
        std::size_t index        = 0;
    };

    State(std::vector<std::string> supported, std::function<void()> notify, Options limits, Scanner load) :
        extensions(std::move(supported)),
        wake(std::move(notify)),
        options(limits),
        scanner(std::move(load))
    {
        for (auto &extension : extensions)
            extension = normalized_extension(std::move(extension));
        std::erase(extensions, std::string{});
        std::sort(extensions.begin(), extensions.end());
        extensions.erase(std::unique(extensions.begin(), extensions.end()), extensions.end());
        if (!scanner)
            scanner = scan_directory;
        worker = std::thread([this] { run(); });
    }

    void run()
    {
        for (;;)
        {
            Task task;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopped.load() || pending.has_value(); });
                if (stopped.load())
                    return;
                task = std::move(*pending);
                pending.reset();
            }
            const Cancelled cancelled = [this, id = task.generation]
            {
                return stopped.load() || generation.load() != id;
            };
            Result result;
            result.generation = task.generation;
            try
            {
                // Resolve the real parent on the worker; lexical folding of a
                // directory symlink followed by ".." changes filesystem meaning.
                const auto parent   = std::filesystem::canonical(task.selected.parent_path(), result.scan.error);
                const auto selected = parent / task.selected.filename();
                if (!result.scan.error)
                    result.scan = scanner(parent, extensions, options, cancelled);
                if (cancelled())
                    continue;
                if (!result.scan.error)
                    finish(result, selected, cancelled);
            } catch (const ScanCancelled &)
            {
                continue;
            } catch (const std::filesystem::filesystem_error &error)
            {
                result.scan.error = error.code();
            } catch (const std::bad_alloc &)
            {
                result.scan.error = std::make_error_code(std::errc::not_enough_memory);
            } catch (...)
            {
                result.scan.error = std::make_error_code(std::errc::io_error);
            }
            if (result.scan.error)
                result.scan.entries.clear();
            {
                std::lock_guard lock(mutex);
                if (cancelled())
                    continue;
                completed = std::move(result);
            }
            // Notification lifetime is bounded by shutdown's join, including a racing close.
            if (wake)
            {
                try
                {
                    wake();
                } catch (...)
                {
                }
            }
        }
    }

    void finish(Result &result, const std::filesystem::path &selected, const Cancelled &cancelled)
    {
        auto &entries     = result.scan.entries;
        std::size_t bytes = 0;
        if (entries.size() > options.max_entries)
        {
            result.scan.error = std::make_error_code(std::errc::value_too_large);
            return;
        }
        for (auto &entry : entries)
        {
            if (cancelled())
                throw ScanCancelled{};
            entry = entry.is_absolute() ? entry : selected.parent_path() / entry;
            if (entry.parent_path() != selected.parent_path())
            {
                result.scan.error = std::make_error_code(std::errc::invalid_argument);
                return;
            }
            if (!add_size(bytes, entry, options))
            {
                result.scan.error = std::make_error_code(std::errc::value_too_large);
                return;
            }
        }
        // Explicit opens remain anchors even without a supported suffix. Other files are filtered by the scanner.
        if (std::find(entries.begin(), entries.end(), selected) == entries.end())
        {
            if (entries.size() >= options.max_entries || !add_size(bytes, selected, options))
            {
                result.scan.error = std::make_error_code(std::errc::value_too_large);
                return;
            }
            entries.push_back(selected);
        }
        std::sort(entries.begin(), entries.end(),
                  [&cancelled](const auto &a, const auto &b)
                  {
                      if (cancelled())
                          throw ScanCancelled{};
                      return natural_less(a, b);
                  });
        entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
        result.index = static_cast<std::size_t>(std::find(entries.begin(), entries.end(), selected) - entries.begin());
    }

    std::vector<std::string> extensions;
    std::function<void()> wake;
    Options options;
    Scanner scanner;
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<bool> stopped{false};
    std::atomic<std::uint64_t> generation{0};
    std::optional<Task> pending;
    std::optional<Result> completed;
    std::thread worker;
};

DirectoryNavigation::DirectoryNavigation(std::vector<std::string> extensions, std::function<void()> wake, Options options,
                                         Scanner scanner) :
    state_(std::make_unique<State>(std::move(extensions), std::move(wake), options, std::move(scanner)))
{
}

DirectoryNavigation::~DirectoryNavigation()
{
    shutdown();
}

void DirectoryNavigation::select(const std::filesystem::path &path)
{
    if (state_->stopped.load())
        return;
    const auto generation = state_->generation.fetch_add(1) + 1;
    entries_.clear();
    index_    = 0;
    snapshot_ = {};
    std::error_code error;
    const auto absolute = path.empty() ? std::filesystem::path{} : std::filesystem::absolute(path, error);
    if (path.empty())
        error = std::make_error_code(std::errc::invalid_argument);
    snapshot_.selected_path = absolute;
    snapshot_.error         = error;
    snapshot_.scanning      = !error;
    {
        std::lock_guard lock(state_->mutex);
        state_->completed.reset();
        state_->pending.reset();
        if (!error)
            state_->pending = State::Task{absolute, generation};
    }
    state_->condition.notify_one();
}

bool DirectoryNavigation::pump()
{
    if (state_->stopped.load())
        return false;
    std::optional<State::Result> result;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->completed)
            return false;
        result = std::move(state_->completed);
        state_->completed.reset();
    }
    if (result->generation != state_->generation.load())
        return false;
    snapshot_.scanning = false;
    snapshot_.error    = result->scan.error;
    entries_           = std::move(result->scan.entries);
    index_             = result->index;
    update_neighbors();
    return true;
}

void DirectoryNavigation::update_neighbors()
{
    snapshot_.current = snapshot_.total = 0;
    snapshot_.previous_path.reset();
    snapshot_.next_path.reset();
    snapshot_.can_previous = snapshot_.can_next = false;
    if (snapshot_.error || entries_.empty() || index_ >= entries_.size())
        return;
    snapshot_.selected_path = entries_[index_];
    snapshot_.current       = index_ + 1;
    snapshot_.total         = entries_.size();
    if (index_ > 0)
        snapshot_.previous_path = entries_[index_ - 1];
    if (index_ + 1 < entries_.size())
        snapshot_.next_path = entries_[index_ + 1];
    snapshot_.can_previous = snapshot_.previous_path.has_value();
    snapshot_.can_next     = snapshot_.next_path.has_value();
}

std::optional<std::filesystem::path> DirectoryNavigation::step(int direction)
{
    if (state_->stopped.load() || snapshot_.scanning || snapshot_.error || entries_.empty() ||
        (direction != -1 && direction != 1))
        return std::nullopt;
    if (direction == -1 ? index_ == 0 : index_ + 1 >= entries_.size())
        return std::nullopt;
    if (direction == -1)
        --index_;
    else
        ++index_;
    update_neighbors();
    return snapshot_.selected_path;
}

void DirectoryNavigation::shutdown()
{
    {
        // Change the predicate while holding the waiter's mutex. An atomic
        // flag alone can lose the notification between its check and wait.
        std::lock_guard lock(state_->mutex);
        if (state_->stopped.exchange(true))
            return;
        state_->pending.reset();
    }
    state_->condition.notify_all();
    if (state_->worker.joinable())
        state_->worker.join();
    {
        std::lock_guard lock(state_->mutex);
        state_->pending.reset();
        state_->completed.reset();
    }
    entries_.clear();
    snapshot_.scanning = false;
    snapshot_.error    = std::make_error_code(std::errc::operation_canceled);
    update_neighbors();
}
} // namespace liltview
