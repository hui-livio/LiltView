#include "platform/fonts.hpp"
#include "settings/store.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#ifdef __linux__
    #include <csignal>
    #include <sys/resource.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace
{
struct TestDirectory
{
    std::filesystem::path path;
    TestDirectory()
    {
        std::random_device random;
        path = std::filesystem::temp_directory_path() / ("liltview-resource-test-" + std::to_string(random()));
        if (!std::filesystem::create_directory(path))
            throw std::runtime_error("Cannot create test directory");
    }
    ~TestDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

struct FontContext
{
    FontContext()
    {
        ImGui::CreateContext();
    }
    ~FontContext()
    {
        ImGui::DestroyContext();
    }
};

std::string read_file(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}
#ifdef __linux__
bool limited_write_preserves_file(const std::filesystem::path &path)
{
    // A real write/flush failure, isolated from the parent's resource limits.
    const auto child = fork();
    if (child < 0)
        return false;
    if (child == 0)
    {
        std::signal(SIGXFSZ, SIG_IGN);
        const rlimit limit{16, 16};
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0)
            _exit(2);
        std::error_code error;
        const bool saved = liltview::save_settings_file(path, {}, error);
        _exit(!saved && error && read_file(path) == "previous" ? 0 : 1);
    }
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif
} // namespace

TEST_CASE("settings replacement preserves complete contents and permissions", "[settings][io]")
{
    TestDirectory directory;
    const auto path = directory.path / "settings.yaml";
    std::error_code error;
    liltview::Settings settings;
    REQUIRE(liltview::save_settings_file(path, settings, error));
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    const auto original_permissions = std::filesystem::status(path).permissions();
    settings.theme = liltview::Theme::light;
    REQUIRE(liltview::save_settings_file(path, settings, error));
    CHECK_FALSE(error);
    CHECK(read_file(path) == liltview::serialize_settings(settings));
    CHECK(std::filesystem::status(path).permissions() == original_permissions);
    CHECK(std::distance(std::filesystem::directory_iterator(directory.path), std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("invalid settings destinations fail without truncating other content", "[settings][io]")
{
    TestDirectory directory;
    std::error_code error;
    REQUIRE_FALSE(liltview::save_settings_file(directory.path, {}, error));
    CHECK(error);
    REQUIRE_FALSE(liltview::save_settings_file(directory.path / "missing" / "settings.yaml", {}, error));
    CHECK(error);
    CHECK(std::filesystem::is_directory(directory.path));
#ifdef __linux__
    const auto previous = directory.path / "settings.yaml";
    std::ofstream(previous) << "previous";
    CHECK(limited_write_preserves_file(previous));
    CHECK(read_file(previous) == "previous");
    CHECK(std::distance(std::filesystem::directory_iterator(directory.path), std::filesystem::directory_iterator{}) == 1);
#endif
}

#ifdef __linux__
TEST_CASE("settings reject special devices and preserve symlinked regular settings", "[settings][io]")
{
    TestDirectory directory;
    std::error_code error;
    const auto link = directory.path / "settings.yaml";
    std::filesystem::create_symlink("/dev/full", link);
    REQUIRE_FALSE(liltview::save_settings_file(link, {}, error));
    CHECK(error);
    CHECK(std::filesystem::is_symlink(link));
    std::filesystem::remove(link);
    const auto target = directory.path / "target.yaml";
    std::ofstream(target) << "theme: dark\n";
    std::filesystem::create_symlink(target, link);
    liltview::Settings settings;
    settings.theme = liltview::Theme::light;
    REQUIRE(liltview::save_settings_file(link, settings, error));
    CHECK(std::filesystem::is_symlink(link));
    CHECK(read_file(target) == liltview::serialize_settings(settings));
}

TEST_CASE("dangling settings symlinks do not write into the working directory", "[settings][io]")
{
    TestDirectory directory;
    const auto link = directory.path / "settings.yaml";
    std::filesystem::create_symlink(directory.path / "missing.yaml", link);
    std::error_code error;
    CHECK_FALSE(liltview::save_settings_file(link, {}, error));
    CHECK(error);
    CHECK_FALSE(std::filesystem::exists(directory.path / "missing.yaml"));
}
#endif

TEST_CASE("font loading rejects empty and corrupted bundled data before ImGui parsing", "[font]")
{
    TestDirectory directory;
    FontContext context;
    const auto path = directory.path / "font.ttf";
    std::ofstream(path) << "";
    CHECK_FALSE(liltview::load_font_candidate({path.string(), 0, true, true}));
    std::ofstream(path, std::ios::binary) << std::string(256, 'x');
    CHECK_FALSE(liltview::load_font_candidate({path.string(), 0, true, true}));
    CHECK(ImGui::GetIO().ConfigErrorRecoveryEnableAssert);
    CHECK(ImGui::GetIO().Fonts->Fonts.empty());
    const auto source = std::filesystem::path(LILTVIEW_TEST_FONT_DIR) / "HarmonyOS_Sans_SC_Regular.ttf";
    std::filesystem::copy_file(source, path, std::filesystem::copy_options::overwrite_existing);
    std::fstream altered(path, std::ios::binary | std::ios::in | std::ios::out);
    altered.put('\xff');
    altered.close();
    CHECK_FALSE(liltview::load_font_candidate({path.string(), 0, true, true}));
}

TEST_CASE("original bundled font loads after a failed candidate", "[font]")
{
    FontContext context;
    CHECK_FALSE(liltview::load_font_candidate({"/missing/font.ttf", 0, false, false}));
    const auto source = std::filesystem::path(LILTVIEW_TEST_FONT_DIR) / "HarmonyOS_Sans_SC_Regular.ttf";
    REQUIRE(liltview::load_font_candidate({source.string(), 0, true, true}));
    CHECK(ImGui::GetIO().Fonts->Fonts.Size == 1);
    CHECK(ImGui::GetIO().ConfigErrorRecoveryEnableAssert);
}
