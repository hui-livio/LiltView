#ifndef LILTVIEW_NAVIGATION_HPP
#define LILTVIEW_NAVIGATION_HPP

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace liltview
{
struct DirectoryNavigationOptions
{
    std::size_t max_entries    = 100000;
    std::size_t max_path_bytes = 32 * 1024 * 1024;
};

struct DirectoryNavigationSnapshot
{
    // The requested file, which can differ from an older image still displayed.
    std::filesystem::path selected_path;
    std::optional<std::filesystem::path> previous_path, next_path;
    std::size_t current = 0, total = 0; // One-based current; both zero until a complete scan succeeds.
    bool scanning = false, can_previous = false, can_next = false;
    std::error_code error;
};

struct DirectoryScanResult
{
    std::vector<std::filesystem::path> entries;
    std::error_code error;
};

// UI-thread model. Filesystem enumeration and sorting run on one private worker.
// The injected scanner must observe cancelled and must not access UI objects.
class DirectoryNavigation
{
public:
    using Options    = DirectoryNavigationOptions;
    using Snapshot   = DirectoryNavigationSnapshot;
    using ScanResult = DirectoryScanResult;
    using Cancelled  = std::function<bool()>;
    using Scanner    = std::function<ScanResult(const std::filesystem::path &, const std::vector<std::string> &, const Options &,
                                                const Cancelled &)>;

    explicit DirectoryNavigation(std::vector<std::string> extensions, std::function<void()> wake = {}, Options options = {},
                                 Scanner scanner = {});
    ~DirectoryNavigation();
    DirectoryNavigation(const DirectoryNavigation &)            = delete;
    DirectoryNavigation &operator=(const DirectoryNavigation &) = delete;

    // An explicit open always refreshes its directory, including the same file.
    void select(const std::filesystem::path &);
    bool pump();
    // Only -1 / +1 are accepted. Selection changes immediately, without rescanning or wrapping.
    std::optional<std::filesystem::path> step(int direction);
    const Snapshot &snapshot() const noexcept
    {
        return snapshot_;
    }
    // Cancels queued/current work and joins before the notification target can disappear.
    void shutdown();

private:
    void update_neighbors();
    struct State;
    std::unique_ptr<State> state_;
    Snapshot snapshot_;
    std::vector<std::filesystem::path> entries_;
    std::size_t index_ = 0;
};
} // namespace liltview
#endif
