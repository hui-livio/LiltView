#include "viewer/navigation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#if defined(__linux__)
    #include <sys/stat.h>
#endif

using namespace liltview;
using namespace std::chrono_literals;

namespace
{
struct Directory
{
    Directory()
    {
        static std::atomic<unsigned> next{0};
        path = std::filesystem::current_path() /
            ("liltview-navigation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
             std::to_string(next.fetch_add(1)));
        std::filesystem::create_directories(path);
    }
    ~Directory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path file(const std::string &name)
    {
        const auto result = path / name;
        std::ofstream(result) << "navigation fixture";
        return result;
    }
    std::filesystem::path path;
};

void settle(DirectoryNavigation &navigation)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (navigation.snapshot().scanning && std::chrono::steady_clock::now() < deadline)
    {
        navigation.pump();
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE_FALSE(navigation.snapshot().scanning);
}

std::vector<std::string> forward(DirectoryNavigation &navigation)
{
    std::vector<std::string> result{navigation.snapshot().selected_path.filename().string()};
    while (const auto next = navigation.step(1))
        result.push_back(next->filename().string());
    return result;
}
} // namespace

TEST_CASE("directory navigation sorts numeric runs naturally with deterministic ties", "[navigation]")
{
    Directory directory;
    const std::vector<std::string> expected{"Image1.png",
                                            "image001.png",
                                            "image01.png",
                                            "image1.png",
                                            "image1a.png",
                                            "image01b.png",
                                            "image2.png",
                                            "image10.png",
                                            "image999999999999999999999.png",
                                            "image1000000000000000000000.png",
                                            "图 2.png",
                                            "图 10.png"};
    for (auto iterator = expected.rbegin(); iterator != expected.rend(); ++iterator)
        directory.file(*iterator);
    DirectoryNavigation navigation({"png"});
    navigation.select(directory.path / expected.front());
    settle(navigation);
    REQUIRE_FALSE(navigation.snapshot().error);
    CHECK(forward(navigation) == expected);
    CHECK_FALSE(navigation.step(1));
    CHECK(navigation.snapshot().current == expected.size());
    while (navigation.step(-1))
    {
    }
    CHECK(navigation.snapshot().selected_path.filename() == expected.front());
    CHECK_FALSE(navigation.step(-1));
}

TEST_CASE("directory navigation filters only provided formats and regular files", "[navigation]")
{
    Directory directory;
    const auto selected = directory.file("a.JPG");
    for (const auto *name : {"b.JpEg", "c.PNG", "d.svg", "e.webp", "f.bmp", "g.GIF", "h.tif", "i.TIFF", ".hidden.png"})
        directory.file(name);
    directory.file("unsupported.avif");
    directory.file("notes.txt");
    std::filesystem::create_directory(directory.path / "folder.png");
    std::filesystem::create_symlink("a.JPG", directory.path / "linked.png");
    std::filesystem::create_symlink("absent", directory.path / "broken.png");
#if defined(__linux__)
    REQUIRE(mkfifo((directory.path / "pipe.png").c_str(), 0600) == 0);
#endif
    DirectoryNavigation navigation({"jpg", ".JPEG", "png", "svg", "webp", "bmp", "gif", "tif", "tiff", "PNG", ""});
    navigation.select(selected);
    settle(navigation);
    REQUIRE_FALSE(navigation.snapshot().error);
    CHECK(navigation.snapshot().total == 11);
    CHECK(navigation.snapshot().current == 2); // Hidden regular image precedes a.JPG.
    while (navigation.step(-1))
    {
    }
    const auto names = forward(navigation);
    CHECK(std::find(names.begin(), names.end(), "linked.png") != names.end());
    CHECK(std::find(names.begin(), names.end(), "unsupported.avif") == names.end());
    CHECK(std::find(names.begin(), names.end(), "folder.png") == names.end());
    CHECK(std::find(names.begin(), names.end(), "broken.png") == names.end());
    CHECK(std::find(names.begin(), names.end(), "pipe.png") == names.end());
}

TEST_CASE("directory selection has exact boundaries and refreshes only explicit opens", "[navigation]")
{
    Directory directory;
    const auto first = directory.file("1.png");
    DirectoryNavigation navigation({"png"});
    CHECK_FALSE(navigation.step(1));
    CHECK(navigation.snapshot().total == 0);
    navigation.select(first);
    CHECK(navigation.snapshot().scanning);
    CHECK(navigation.snapshot().current == 0);
    CHECK_FALSE(navigation.step(1));
    settle(navigation);
    CHECK(navigation.snapshot().current == 1);
    CHECK(navigation.snapshot().total == 1);
    CHECK_FALSE(navigation.snapshot().can_previous);
    CHECK_FALSE(navigation.snapshot().can_next);
    CHECK_FALSE(navigation.step(-1));
    CHECK_FALSE(navigation.step(0));
    CHECK_FALSE(navigation.step(2));
    const auto second = directory.file("2.png");
    const auto third  = directory.file("3.png");
    CHECK_FALSE(navigation.step(1)); // Directory mutation is adopted by refresh, not a synchronous step.
    navigation.select(first);
    settle(navigation);
    CHECK(navigation.snapshot().total == 3);
    CHECK(navigation.snapshot().next_path == second);
    CHECK(navigation.step(1) == second);
    CHECK(navigation.snapshot().current == 2);
    CHECK(navigation.snapshot().previous_path == first);
    CHECK(navigation.snapshot().next_path == third);
    CHECK(navigation.step(1) == third);
    CHECK_FALSE(navigation.snapshot().can_next);
    CHECK(navigation.step(-1) == second);
    CHECK_FALSE(navigation.snapshot().scanning);
    std::filesystem::remove(first);
    navigation.select(second);
    settle(navigation);
    CHECK(navigation.snapshot().current == 1);
    CHECK(navigation.snapshot().total == 2);
}

TEST_CASE("explicit files without a supported suffix remain navigation anchors", "[navigation]")
{
    Directory directory;
    const auto first    = directory.file("1.png");
    const auto selected = directory.file("2 content with 中文");
    const auto last     = directory.file("3.png");
    directory.file("other.unknown");
    DirectoryNavigation navigation({"png"});
    navigation.select(selected.parent_path() / "." / selected.filename());
    settle(navigation);
    CHECK(navigation.snapshot().selected_path == selected);
    CHECK(navigation.snapshot().current == 2);
    CHECK(navigation.snapshot().total == 3);
    CHECK(navigation.snapshot().previous_path == first);
    CHECK(navigation.snapshot().next_path == last);
    CHECK(navigation.step(1) == last);
    CHECK(navigation.step(-1) == selected);
    DirectoryNavigation no_formats({});
    no_formats.select(selected);
    settle(no_formats);
    CHECK(no_formats.snapshot().total == 1);
}

TEST_CASE("relative navigation paths become absolute without requiring decode completion", "[navigation]")
{
    Directory directory;
    const auto first = directory.file("1.png"), second = directory.file("2.png"), third = directory.file("3.png");
    DirectoryNavigation navigation({"png"});
    navigation.select(first.lexically_relative(std::filesystem::current_path()));
    settle(navigation);
    CHECK(navigation.snapshot().selected_path == first);
    CHECK(navigation.step(1) == second);
    CHECK(navigation.step(1) == third);
    CHECK(navigation.step(-1) == second);
    std::filesystem::remove(third);
    CHECK(navigation.step(1) == third); // A failed image open does not strand the directory cursor.
    CHECK(navigation.step(-1) == second);
}

TEST_CASE("navigation resolves parent symlinks before dot dot without choosing a different directory", "[navigation][path]")
{
#ifdef _WIN32
    SKIP("Win32 resolves dot-dot before following directory symlinks; this is a POSIX path contract");
#endif
    Directory directory;
    const auto local  = directory.file("photo.png");
    const auto remote = directory.path / "remote";
    std::filesystem::create_directories(remote / "child");
    std::ofstream(remote / "photo.png") << "different image";
    std::ofstream(remote / "second.png") << "neighbor";
    std::filesystem::create_directory_symlink(remote / "child", directory.path / "link");
    DirectoryNavigation navigation({"png"});
    navigation.select(directory.path / "link" / ".." / "photo.png");
    settle(navigation);
    REQUIRE_FALSE(navigation.snapshot().error);
    CHECK(navigation.snapshot().selected_path == remote / "photo.png");
    CHECK(navigation.snapshot().selected_path != local);
    CHECK(navigation.snapshot().next_path == remote / "second.png");
    CHECK(navigation.snapshot().total == 2);
}

TEST_CASE("blocked old directory scans never block UI operations or replace a new directory", "[navigation][thread]")
{
    Directory old_directory, new_directory;
    const auto old_file = old_directory.file("old.png"), next_file = new_directory.file("new.png");
    std::promise<void> entered;
    auto started = entered.get_future();
    std::promise<void> notified;
    auto notification = notified.get_future();
    std::atomic<unsigned> calls{0}, wakes{0};
    DirectoryNavigation navigation(
        {"png"},
        [&]
        {
            ++wakes;
            notified.set_value();
        },
        {},
        [&](const auto &directory, const auto &, const auto &, const DirectoryNavigation::Cancelled &cancelled)
        {
            ++calls;
            if (directory == old_directory.path)
            {
                entered.set_value();
                while (!cancelled())
                    std::this_thread::sleep_for(1ms);
                return DirectoryScanResult{{old_file}, {}};
            }
            return DirectoryScanResult{{next_file}, {}};
        });
    navigation.select(old_file);
    REQUIRE(started.wait_for(5s) == std::future_status::ready);
    CHECK_FALSE(navigation.pump());
    CHECK_FALSE(navigation.step(1));
    navigation.select(next_file); // Must not wait for the active scanner to return.
    settle(navigation);
    REQUIRE(notification.wait_for(5s) == std::future_status::ready);
    CHECK(navigation.snapshot().selected_path == next_file);
    CHECK(navigation.snapshot().total == 1);
    CHECK(calls.load() == 2);
    CHECK(wakes.load() == 1);
    CHECK_FALSE(navigation.pump());
}

TEST_CASE("completed scans are discarded when a newer selection is made before pump", "[navigation][thread]")
{
    Directory directory;
    const auto first = directory.file("1.png"), second = directory.file("2.png");
    std::mutex mutex;
    std::condition_variable condition;
    unsigned wakes = 0;
    DirectoryNavigation navigation({"png"},
                                   [&]
                                   {
                                       std::lock_guard lock(mutex);
                                       ++wakes;
                                       condition.notify_all();
                                   });
    navigation.select(first);
    {
        std::unique_lock lock(mutex);
        REQUIRE(condition.wait_for(lock, 5s, [&] { return wakes == 1; }));
    }
    navigation.select(second);
    settle(navigation);
    CHECK(navigation.snapshot().selected_path == second);
    CHECK(navigation.snapshot().current == 2);
    CHECK(navigation.snapshot().total == 2);
}

TEST_CASE("directory failures and scanner exceptions never publish partial totals", "[navigation]")
{
    Directory directory;
    const auto selected = directory.file("selected.png");
    for (const bool throws : {false, true})
    {
        DirectoryNavigation navigation({"png"}, {}, {},
                                       [&, throws](const auto &, const auto &, const auto &, const auto &) -> DirectoryScanResult
                                       {
                                           if (throws)
                                               throw std::runtime_error("scanner failure");
                                           return {{selected}, std::make_error_code(std::errc::permission_denied)};
                                       });
        navigation.select(selected);
        settle(navigation);
        CHECK(navigation.snapshot().selected_path == selected);
        CHECK(navigation.snapshot().error == std::make_error_code(throws ? std::errc::io_error : std::errc::permission_denied));
        CHECK(navigation.snapshot().total == 0);
        CHECK(navigation.snapshot().current == 0);
        CHECK_FALSE(navigation.snapshot().previous_path);
        CHECK_FALSE(navigation.snapshot().next_path);
        CHECK_FALSE(navigation.step(1));
    }
    DirectoryNavigation absent({"png"});
    absent.select(directory.path / "absent" / "file.png");
    settle(absent);
    CHECK(absent.snapshot().error == std::errc::no_such_file_or_directory);
    CHECK(absent.snapshot().total == 0);
    absent.select({});
    CHECK(absent.snapshot().error == std::make_error_code(std::errc::invalid_argument));
    CHECK_FALSE(absent.snapshot().scanning);
}

TEST_CASE("directory entry and path budgets include explicit anchors and reject partial lists", "[navigation]")
{
    Directory directory;
    const auto first = directory.file("1.png"), second = directory.file("2.png"), anchor = directory.file("3.raw");
    DirectoryNavigation::Options options;
    options.max_entries = 2;
    DirectoryNavigation limited({"png"}, {}, options);
    limited.select(first);
    settle(limited);
    REQUIRE_FALSE(limited.snapshot().error);
    CHECK(limited.snapshot().total == 2);
    limited.select(anchor);
    settle(limited);
    CHECK(limited.snapshot().error == std::make_error_code(std::errc::value_too_large));
    CHECK(limited.snapshot().total == 0);
    directory.file("4.png");
    limited.select(first);
    settle(limited);
    CHECK(limited.snapshot().error == std::make_error_code(std::errc::value_too_large));
    CHECK_FALSE(limited.snapshot().can_next);

    options.max_entries    = 10;
    options.max_path_bytes = first.native().size() * sizeof(std::filesystem::path::value_type);
    DirectoryNavigation byte_limited({"png"}, {}, options);
    byte_limited.select(first);
    settle(byte_limited);
    CHECK(byte_limited.snapshot().error == std::make_error_code(std::errc::value_too_large));
    CHECK(byte_limited.snapshot().current == 0);
    CHECK(byte_limited.snapshot().total == 0);
    DirectoryNavigation::Options zero;
    zero.max_entries = 0;
    DirectoryNavigation zero_limited({}, {}, zero);
    zero_limited.select(anchor);
    settle(zero_limited);
    CHECK(zero_limited.snapshot().error == std::make_error_code(std::errc::value_too_large));
    (void)second;
}

TEST_CASE("directory shutdown joins cancelled work and prevents late notifications or new work", "[navigation][thread]")
{
    Directory directory;
    const auto file = directory.file("file.png");
    std::promise<void> entered;
    auto started = entered.get_future();
    std::atomic<unsigned> calls{0}, wakes{0};
    DirectoryNavigation navigation(
        {"png"}, [&] { ++wakes; }, {},
        [&](const auto &, const auto &, const auto &, const DirectoryNavigation::Cancelled &cancelled)
        {
            ++calls;
            entered.set_value();
            while (!cancelled())
                std::this_thread::sleep_for(1ms);
            return DirectoryScanResult{{file}, {}};
        });
    navigation.select(file);
    REQUIRE(started.wait_for(5s) == std::future_status::ready);
    navigation.shutdown();
    navigation.shutdown();
    CHECK(wakes.load() == 0);
    CHECK_FALSE(navigation.snapshot().scanning);
    CHECK(navigation.snapshot().total == 0);
    CHECK(navigation.snapshot().error == std::make_error_code(std::errc::operation_canceled));
    navigation.select(file);
    CHECK_FALSE(navigation.step(1));
    CHECK_FALSE(navigation.pump());
    CHECK(calls.load() == 1);
    CHECK(wakes.load() == 0);
}

TEST_CASE("immediate idle and queued directory shutdown cannot lose the worker wakeup", "[navigation][thread][shutdown]")
{
    Directory directory;
    const auto path = directory.file("image.png");
    for (unsigned iteration = 0; iteration < 300; ++iteration)
    {
        DirectoryNavigation navigation({"png"});
        if (iteration % 2)
            navigation.select(path);
        navigation.shutdown();
        CHECK_FALSE(navigation.snapshot().scanning);
        CHECK_FALSE(navigation.step(1));
    }
}

TEST_CASE("directory shutdown also discards ready results and an idle worker", "[navigation][thread]")
{
    Directory directory;
    const auto file = directory.file("file.png");
    DirectoryNavigation idle({"png"});
    idle.shutdown();
    CHECK_FALSE(idle.snapshot().scanning);
    CHECK_FALSE(idle.pump());

    std::promise<void> notified;
    auto notification = notified.get_future();
    std::atomic<unsigned> wakes{0};
    DirectoryNavigation ready({"png"},
                              [&]
                              {
                                  ++wakes;
                                  notified.set_value();
                              });
    ready.select(file);
    REQUIRE(notification.wait_for(5s) == std::future_status::ready);
    ready.shutdown();
    CHECK_FALSE(ready.snapshot().scanning);
    CHECK(ready.snapshot().total == 0);
    CHECK_FALSE(ready.pump());
    ready.select(file);
    CHECK(wakes.load() == 1);
}
