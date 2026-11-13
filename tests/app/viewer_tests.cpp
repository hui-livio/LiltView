// GIF warm-adoption integration UT scenarios reviewed before implementation:
// 1. Content-classified .png GIF warms from static A without GPU changes, then draws red 0 exactly once and blue 1.
// 2. Same-length palette mutation/deletion after READY rejects stale frame 0 and reopens new content or preserves A on I/O.
// 3. Different framebuffer FIT bounds bypass incompatible READY pixels and use ordinary playback at the new size.
// 4. Warm single-frame GIFs preserve total plays 1/3/infinite, real epoch transitions, and exactly one warmed first frame.
// 5. A verified adoption paused before presentation can be replaced or shut down without leaking its session/pixels.
// 6. Current GIF playback progresses while neighbor warm/error work stays within the global two-request quota.
// 7. Two large READY resident canvases are released before an unrelated larger foreground GIF is admitted.
// 8. Uploads slower than frame duration cannot skip the first actual red draw or present two frames in one installation.
// 9. Failure of only the second candidate upload falls back before commit, with one ordinary first presentation.
// 10. A genuine old static cold-restore failure must not suppress neighbor prefetch after new GIF presentation.

#include "../fixtures/checkerboard.hpp"
#include "viewer/viewer.hpp"

#include <SDL3/SDL.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gif_lib.h>
#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
// Real core and viewer, controlled renderer failures: tests cover ownership and
// navigation without relying on GPU allocation timing or a compositor.
class TestRenderer final : public liltview::render::Renderer
{
public:
    using Image  = liltview::render::Image;
    using Size   = liltview::render::Size;
    using Pixels = liltview::render::Pixels;
    using Rect   = liltview::render::Rect;
    using Point  = liltview::render::Point;
    struct Texture
    {
        Size size;
        std::uint64_t bytes;
        liltview::render::ImageDesc::Magnification magnification;
        std::array<std::uint8_t, 4> first_pixel{};
    };
    std::unordered_map<std::uint32_t, Texture> textures;
    std::uint32_t next = 1;
    bool fail_upload   = false;
    std::optional<unsigned> fail_upload_at;
    unsigned uploads                  = 0;
    unsigned upload_delay_ms          = 0;
    unsigned creates                  = 0;
    std::uint64_t image_budget        = 16 * 1024 * 1024;
    std::uint32_t max_edge            = 4096;
    bool allow_upload                 = true;
    bool strict_image_budget          = false;
    unsigned budget_rejections        = 0;
    std::uint64_t rejected_live_bytes = 0, rejected_request_bytes = 0;
    Size frame_size{128, 96};
    std::optional<std::array<std::uint8_t, 4>> last_drawn_pixel;
    std::vector<std::array<std::uint8_t, 4>> uploaded_first_pixels;
    // Opt-in deferred submission catches images destroyed after Viewer::draw()
    // but before the backend consumes the frame, without a real GPU/window.
    bool record_draws = false;
    std::vector<Image> frame_images;

    bool upload_ready() override
    {
        return allow_upload;
    }

    liltview::render::Capabilities capabilities() const override
    {
        std::uint64_t bytes = 0;
        for (const auto &[id, texture] : textures)
        {
            (void)id;
            bytes += texture.bytes;
        }
        return {liltview::render::Backend::sdl_renderer, max_edge, image_budget, bytes};
    }
    Size framebuffer_size() const override
    {
        return frame_size;
    }
    Image create_image(liltview::render::ImageDesc desc) override
    {
        REQUIRE(desc.size.width <= max_edge);
        REQUIRE(desc.size.height <= max_edge);
        if (strict_image_budget)
        {
            const auto used  = capabilities().image_bytes;
            const auto bytes = liltview::render::image_storage_bytes(desc);
            if (used > image_budget || bytes > image_budget - used)
            {
                ++budget_rejections;
                rejected_live_bytes    = used;
                rejected_request_bytes = bytes;
                throw liltview::render::Error(liltview::render::ErrorCode::memory_budget, "Strict test GPU image budget");
            }
        }
        ++creates;
        const auto slot = next++;
        textures.emplace(slot, Texture{desc.size, liltview::render::image_storage_bytes(desc), desc.magnification});
        return {1, slot, 1};
    }
    void update_image(Image image, Rect rect, Pixels pixels) override
    {
        if (fail_upload || (fail_upload_at && uploads + 1 == *fail_upload_at))
        {
            fail_upload = false;
            fail_upload_at.reset();
            throw liltview::render::Error(liltview::render::ErrorCode::backend_failure, "Injected upload failure");
        }
        liltview::render::validate_upload(textures.at(image.slot).size, rect, pixels);
        if (rect.x == 0 && rect.y == 0)
        {
            std::copy_n(pixels.data, 4, textures.at(image.slot).first_pixel.begin());
            uploaded_first_pixels.push_back(textures.at(image.slot).first_pixel);
        }
        ++uploads;
        if (upload_delay_ms)
            SDL_Delay(upload_delay_ms);
    }
    void destroy_image(Image image) noexcept override
    {
        textures.erase(image.slot);
    }
    void add_image(ImDrawList *, Image image, Point, Point, Point, Point) override
    {
        last_drawn_pixel = textures.at(image.slot).first_pixel;
        if (record_draws)
            frame_images.push_back(image);
    }
    void add_image_quad(ImDrawList *, Image image, const std::array<Point, 4> &, const std::array<Point, 4> &) override
    {
        last_drawn_pixel = textures.at(image.slot).first_pixel;
        if (record_draws)
            frame_images.push_back(image);
    }
    void begin_frame() override
    {
        frame_images.clear();
    }
    void render_frame(ImDrawData *) override
    {
        for (const auto image : frame_images)
        {
            INFO("Submitted image slot: " << image.slot);
            CHECK(textures.contains(image.slot));
        }
        frame_images.clear();
    }
    void present() override
    {
    }
    liltview::render::Readback readback() override
    {
        return {};
    }
};

struct EventLoop
{
    EventLoop()
    {
        if (!SDL_Init(SDL_INIT_EVENTS))
            throw std::runtime_error(SDL_GetError());
    }
    ~EventLoop()
    {
        SDL_Quit();
    }
    void settle(liltview::Viewer &viewer)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do
        {
            (viewer.pump(), viewer.prepare_loading_view());
            if (viewer.settled())
                return;
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Viewer did not settle");
            SDL_Event event;
            SDL_WaitEventTimeout(&event, 10);
        } while (true);
    }
    void settle_prefetch(liltview::Viewer &viewer)
    {
        settle(viewer);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do
        {
            (viewer.pump(), viewer.prepare_loading_view());
            if (!viewer.navigation().scanning && !viewer.prefetch_pending())
                return;
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Viewer neighbor prefetch did not settle");
            SDL_Event event;
            SDL_WaitEventTimeout(&event, 10);
        } while (true);
    }
};

const std::filesystem::path sample = std::filesystem::path(LILTVIEW_JPEG_FIXTURES) / "testorig.jpg";
} // namespace

TEST_CASE("viewer prefetches both neighbors without GPU uploads and navigation reuses core content", "[viewer][prefetch]")
{
    EventLoop loop;
    TestRenderer renderer;
    const auto folder =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("liltview-prefetch-" + std::to_string(SDL_GetPerformanceCounter()));
    std::filesystem::create_directories(folder);
    for (const auto *name : {"1.png", "2.png", "3.png"})
        liltview::test::checkerboard(folder / name, 31, 23, 7);
    liltview::Viewer viewer(renderer);
    viewer.open(folder / "2.png");
    loop.settle(viewer);
    const auto uploads  = renderer.uploads;
    const auto creates  = renderer.creates;
    const auto textures = renderer.textures.size();
    loop.settle_prefetch(viewer);
    CHECK(viewer.prefetch_stats().submitted == 2);
    CHECK(viewer.prefetch_stats().completed == 2);
    CHECK(viewer.prefetch_stats().active == 0);
    CHECK(renderer.uploads == uploads);
    CHECK(renderer.creates == creates);
    CHECK(renderer.textures.size() == textures);
    CHECK(viewer.content_cache_bytes() > 0);
    CHECK(viewer.content_cache_bytes() <= 64 * 1024 * 1024);
    for (unsigned i = 0; i < 10; ++i)
        (viewer.pump(), viewer.prepare_loading_view());
    CHECK(viewer.prefetch_stats().submitted == 2);
    viewer.navigate(1);
    loop.settle(viewer);
    CHECK(viewer.filename() == "3.png");
    CHECK(viewer.last_source_bytes_read() == 0);
    REQUIRE(viewer.image_details());
    CHECK(viewer.image_details()->path == folder / "3.png");
    viewer.navigate(-1);
    viewer.navigate(-1);
    loop.settle(viewer);
    CHECK(viewer.filename() == "1.png");
    CHECK(viewer.last_source_bytes_read() == 0);
    viewer.shutdown();
    std::filesystem::remove_all(folder);
}

TEST_CASE("viewer can shut down accepted neighbor prefetch without polling its terminal", "[viewer][prefetch][shutdown]")
{
    EventLoop loop;
    TestRenderer renderer;
    const auto folder =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("liltview-prefetch-close-" + std::to_string(SDL_GetPerformanceCounter()));
    std::filesystem::create_directories(folder);
    liltview::test::checkerboard(folder / "1.png", 31, 23, 7);
    liltview::test::checkerboard(folder / "2.png", 4096, 2048, 7);
    liltview::Viewer viewer(renderer);
    viewer.open(folder / "1.png");
    loop.settle(viewer);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!viewer.prefetch_stats().active)
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        (viewer.pump(), viewer.prepare_loading_view());
        SDL_Delay(1);
    }
    viewer.begin_shutdown();
    REQUIRE(viewer.join_shutdown(5000));
    viewer.shutdown();
    CHECK_FALSE(viewer.prefetch_pending());
    CHECK(viewer.prefetch_stats().active == 0);
    CHECK(viewer.content_cache_bytes() == 0);
    CHECK(renderer.textures.empty());
    std::filesystem::remove_all(folder);
}

TEST_CASE("cross directory generation churn isolates retired neighbor results from the final image", "[viewer][prefetch]")
{
    EventLoop loop;
    TestRenderer renderer;
    const auto folder =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("liltview-prefetch-generations-" + std::to_string(SDL_GetPerformanceCounter()));
    for (const auto *name : {"A", "B", "C"})
    {
        std::filesystem::create_directories(folder / name);
        liltview::test::checkerboard(folder / name / "1.png", 31, 23, 7);
        liltview::test::checkerboard(folder / name / "2.png", 1024, 1024, 7);
    }
    liltview::Viewer viewer(renderer);
    viewer.open(folder / "A" / "1.png");
    loop.settle(viewer);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!viewer.prefetch_stats().active)
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        (viewer.pump(), viewer.prepare_loading_view());
        SDL_Delay(1);
    }
    const auto old_submitted = viewer.prefetch_stats().submitted;
    REQUIRE(old_submitted > 0);
    viewer.open(folder / "B" / "1.png");
    (viewer.pump(), viewer.prepare_loading_view());
    viewer.open(folder / "C" / "1.png");
    loop.settle_prefetch(viewer);
    CHECK_FALSE(viewer.failed());
    REQUIRE(viewer.image_details());
    CHECK(viewer.image_details()->path == folder / "C" / "1.png");
    CHECK(viewer.navigation().selected_path == folder / "C" / "1.png");
    CHECK(viewer.prefetch_stats().active <= 2);
    CHECK(viewer.prefetch_stats().submitted > old_submitted);
    viewer.navigate(1);
    loop.settle(viewer);
    CHECK(viewer.last_source_bytes_read() == 0);
    CHECK(viewer.image_details()->path == folder / "C" / "2.png");
    viewer.shutdown();
    std::filesystem::remove_all(folder);
}

TEST_CASE("same size neighbor rewrites display new pixels instead of cached content", "[viewer][prefetch][source]")
{
    EventLoop loop;
    TestRenderer renderer;
    const auto folder =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("liltview-prefetch-pixels-" + std::to_string(SDL_GetPerformanceCounter()));
    std::filesystem::create_directories(folder);
    liltview::test::checkerboard(folder / "1.bmp", 31, 23, 7);
    liltview::test::checkerboard(folder / "2.bmp", 31, 23, 7);
    liltview::Viewer viewer(renderer);
    viewer.open(folder / "1.bmp");
    loop.settle_prefetch(viewer);
    REQUIRE(renderer.textures.size() == 1);
    CHECK(renderer.textures.begin()->second.first_pixel == std::array<std::uint8_t, 4>{255, 0, 0, 255});
    {
        // The 8-bit fixture's first palette entry is BGRA at byte 54. Keep dimensions/file length.
        std::fstream file(folder / "2.bmp", std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(file.good());
        file.seekp(54);
        const std::array<char, 4> blue{static_cast<char>(255), 0, 0, 0};
        file.write(blue.data(), static_cast<std::streamsize>(blue.size()));
        file.flush();
        REQUIRE(file.good());
    }
    viewer.navigate(1);
    loop.settle(viewer);
    REQUIRE_FALSE(viewer.failed());
    CHECK(viewer.last_source_bytes_read() > 0);
    REQUIRE(renderer.textures.size() == 1);
    CHECK(renderer.textures.begin()->second.first_pixel == std::array<std::uint8_t, 4>{0, 0, 255, 255});
    viewer.shutdown();
    std::filesystem::remove_all(folder);
}

TEST_CASE("viewer neighbor boundaries and failed background reads preserve the current image", "[viewer][prefetch]")
{
    EventLoop loop;
    TestRenderer renderer;
    const auto folder =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("liltview-prefetch-errors-" + std::to_string(SDL_GetPerformanceCounter()));
    std::filesystem::create_directories(folder);
    liltview::test::checkerboard(folder / "1.png", 31, 23, 7);
    liltview::Viewer viewer(renderer);
    viewer.open(folder / "1.png");
    loop.settle_prefetch(viewer);
    CHECK(viewer.prefetch_stats().submitted == 0);
    std::ofstream(folder / "2.png") << "not an encoded image";
    viewer.open(folder / "1.png");
    loop.settle_prefetch(viewer);
    CHECK(viewer.prefetch_stats().submitted == 1);
    CHECK(viewer.prefetch_stats().failed == 1);
    CHECK_FALSE(viewer.failed());
    REQUIRE(viewer.image_details());
    CHECK(viewer.image_details()->path == folder / "1.png");
    for (unsigned i = 0; i < 10; ++i)
        (viewer.pump(), viewer.prepare_loading_view());
    CHECK(viewer.prefetch_stats().submitted == 1);
    CHECK(viewer.prefetch_stats().failed == 1);
    viewer.shutdown();
    std::filesystem::remove_all(folder);
}

TEST_CASE("viewer revalidates a prefetched neighbor before displaying changed or deleted source", "[viewer][prefetch][source]")
{
    EventLoop loop;
    for (const bool erase : {false, true})
    {
        TestRenderer renderer;
        const auto folder =
            std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("liltview-prefetch-changed-" + std::to_string(SDL_GetPerformanceCounter()));
        std::filesystem::create_directories(folder);
        liltview::test::checkerboard(folder / "1.png", 31, 23, 7);
        liltview::test::checkerboard(folder / "2.png", 31, 23, 7);
        liltview::Viewer viewer(renderer);
        viewer.open(folder / "1.png");
        loop.settle_prefetch(viewer);
        REQUIRE(viewer.prefetch_stats().completed == 1);
        if (erase)
            std::filesystem::remove(folder / "2.png");
        else
            liltview::test::checkerboard(folder / "2.png", 61, 47, 7);
        viewer.navigate(1);
        loop.settle(viewer);
        if (erase)
        {
            CHECK(viewer.error() == LV_ERROR_IO);
            CHECK(viewer.image_details()->path == folder / "1.png");
        }
        else
        {
            CHECK_FALSE(viewer.failed());
            CHECK(viewer.last_source_bytes_read() > 0);
            CHECK(viewer.source_size().width == 61);
            CHECK(viewer.source_size().height == 47);
        }
        viewer.shutdown();
        std::filesystem::remove_all(folder);
    }
}

TEST_CASE("background neighbor events never replace GIF frame identity or foreground errors", "[viewer][prefetch][animation]")
{
    EventLoop loop;
    TestRenderer renderer;
    const auto folder =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("liltview-prefetch-gif-" + std::to_string(SDL_GetPerformanceCounter()));
    std::filesystem::create_directories(folder);
    std::filesystem::copy_file(std::filesystem::path(LILTVIEW_IMAGE_FIXTURES) / "red-blue-loop.gif", folder / "1.gif");
    std::ofstream(folder / "2.png") << "not an encoded image";
    liltview::Viewer viewer(renderer);
    viewer.open(folder / "1.gif");
    loop.settle(viewer);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool later_frame    = false;
    while (!later_frame || !viewer.prefetch_stats().failed)
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        (viewer.pump(), viewer.prepare_loading_view());
        later_frame = later_frame || viewer.displayed_frame_index() == 1;
        CHECK_FALSE(viewer.failed());
        REQUIRE(viewer.image_details());
        CHECK(viewer.image_details()->image.format == LV_FORMAT_GIF);
        CHECK(viewer.image_details()->path == folder / "1.gif");
        SDL_Delay(1);
    }
    CHECK(viewer.prefetch_stats().submitted == 1);
    viewer.shutdown();
    std::filesystem::remove_all(folder);
}

TEST_CASE("viewer browses a naturally sorted directory and can leave a deleted or failed target", "[viewer][navigation]")
{
    EventLoop loop;
    TestRenderer renderer;
    const auto folder =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("liltview-navigation-" + std::to_string(SDL_GetPerformanceCounter()));
    std::filesystem::create_directories(folder);
    for (const auto *name : {"image1.png", "image2.PNG", "image10.png"})
    {
        const auto generated = folder / "generated.png";
        liltview::test::checkerboard(generated, 17, 13, 1);
        std::filesystem::rename(generated, folder / name);
    }
    liltview::Viewer viewer(renderer);
    viewer.open(folder / "image1.png");
    loop.settle(viewer);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (viewer.navigation().scanning)
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        (viewer.pump(), viewer.prepare_loading_view());
        SDL_Delay(1);
    }
    REQUIRE(viewer.navigation().total == 3);
    CHECK(viewer.navigation().current == 1);
    CHECK_FALSE(viewer.navigation().can_previous);
    REQUIRE(viewer.navigation().can_next);
    viewer.navigate(1);
    CHECK(viewer.navigation().current == 2);
    viewer.navigate(1);
    viewer.navigate(-1);
    loop.settle(viewer);
    CHECK(viewer.filename() == "image2.PNG");
    CHECK_FALSE(viewer.failed());
    std::filesystem::remove(folder / "image10.png");
    viewer.navigate(1);
    loop.settle(viewer);
    CHECK(viewer.error() == LV_ERROR_IO);
    CHECK(viewer.navigation().current == 3);
    CHECK_FALSE(viewer.navigation().can_next);
    viewer.navigate(1);
    CHECK(viewer.navigation().current == 3);
    viewer.navigate(-1);
    loop.settle(viewer);
    CHECK_FALSE(viewer.failed());
    CHECK(viewer.filename() == "image2.PNG");
    viewer.shutdown();
    std::filesystem::remove_all(folder);
}

TEST_CASE("viewer preserves filesystem meaning of a parent symlink followed by dot dot", "[viewer][navigation][path]")
{
#ifdef _WIN32
    SKIP("Win32 resolves dot-dot before following directory symlinks; this is a POSIX path contract");
#endif
    EventLoop loop;
    TestRenderer renderer;
    const auto folder =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("liltview-navigation-symlink-" + std::to_string(SDL_GetPerformanceCounter()));
    const auto remote = folder / "remote";
    std::filesystem::create_directories(remote / "child");
    liltview::test::checkerboard(folder / "photo.png", 17, 13, 1);
    liltview::test::checkerboard(remote / "photo.png", 23, 19, 1);
    std::filesystem::create_directory_symlink(remote / "child", folder / "link");
    liltview::Viewer viewer(renderer);
    viewer.open(folder / "link" / ".." / "photo.png");
    loop.settle(viewer);
    REQUIRE_FALSE(viewer.failed());
    CHECK(viewer.source_size().width == 23);
    CHECK(viewer.source_size().height == 19);
    viewer.shutdown();
    std::filesystem::remove_all(folder);
}

TEST_CASE("canvas navigation interaction blocks real image wheel and drag without disabling normal input",
          "[viewer][navigation_buttons]")
{
    EventLoop loop;
    TestRenderer renderer;
    renderer.image_budget = 128 * 1024 * 1024; // Keep this input-control test fully resident.
    const auto folder =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("liltview-navigation-input-" + std::to_string(SDL_GetPerformanceCounter()));
    std::filesystem::create_directories(folder);
    liltview::test::checkerboard(folder / "board.png", 1024, 1024, 17);
    liltview::Viewer viewer(renderer);
    viewer.open(folder / "board.png");
    loop.settle(viewer);
    struct Gui
    {
        Gui()
        {
            ImGui::CreateContext();
            auto &io       = ImGui::GetIO();
            io.DisplaySize = {320, 240};
            io.DeltaTime   = 1.0f / 60;
            io.IniFilename = io.LogFilename = nullptr;
            io.Fonts->AddFontDefault();
            unsigned char *pixels = nullptr;
            int width = 0, height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            io.Fonts->SetTexID(1);
        }
        ~Gui()
        {
            ImGui::DestroyContext();
        }
    } gui;
    const auto frame = [&](bool interactive)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({320, 240});
        ImGui::Begin("navigation_image_input", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        viewer.draw({0, 0}, {128, 96}, 1, interactive);
        ImGui::End();
        ImGui::Render();
    };
    viewer.set_viewport({0, 0}, {128, 96});
    viewer.actual_size();
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(64, 48);
    frame(true);
    frame(true);
    const auto original = viewer.presented_view();
    io.AddMouseWheelEvent(0, 1);
    frame(false);
    CHECK(viewer.presented_view().same_view(original));
    io.AddMouseButtonEvent(0, true);
    frame(false);
    io.AddMousePosEvent(90, 70);
    frame(false);
    CHECK(viewer.presented_view().same_view(original));
    io.AddMouseButtonEvent(0, false);
    frame(false);
    CHECK(viewer.presented_view().same_view(original));
    io.AddMouseWheelEvent(0, 1);
    frame(true);
    CHECK_FALSE(viewer.presented_view().same_view(original));
    const auto zoomed = viewer.presented_view();
    io.AddMouseButtonEvent(0, true);
    frame(true);
    io.AddMousePosEvent(110, 85);
    frame(true);
    CHECK_FALSE(viewer.presented_view().same_view(zoomed));
    io.AddMouseButtonEvent(0, false);
    frame(true);
    viewer.shutdown();
    std::filesystem::remove_all(folder);
}

TEST_CASE("viewer replaces in-flight requests and releases every renderer image", "[viewer]")
{
    EventLoop loop;
    TestRenderer renderer;
    liltview::Viewer viewer(renderer);
    viewer.open(sample);
    (viewer.pump(), viewer.prepare_loading_view());
    viewer.open(sample.parent_path() / "testimgint.jpg");
    loop.settle(viewer);
    REQUIRE(viewer.has_image());
    CHECK_FALSE(viewer.failed());
    CHECK(viewer.filename() == "testimgint.jpg");
    CHECK(renderer.uploads > 0);
    CHECK(renderer.textures.size() == 1);
    viewer.begin_shutdown();
    viewer.shutdown();
    CHECK(renderer.textures.empty());
}

TEST_CASE("viewer shutdown joins idle and in-flight workers with a bounded wait", "[viewer][shutdown]")
{
    EventLoop loop;
    for (const bool open_image : {false, true})
    {
        CAPTURE(open_image);
        TestRenderer renderer;
        liltview::Viewer viewer(renderer);
        if (open_image)
        {
            viewer.open(sample);
            (viewer.pump(), viewer.prepare_loading_view());
        }
        viewer.begin_shutdown();
        viewer.begin_shutdown();
        REQUIRE(viewer.join_shutdown(5000));
        CHECK(viewer.join_shutdown());
        viewer.shutdown();
        CHECK(viewer.join_shutdown(10));
        CHECK(renderer.textures.empty());
    }
}

TEST_CASE("viewer recovers from upload failure and from an invalid file", "[viewer]")
{
    EventLoop loop;
    TestRenderer renderer;
    liltview::Viewer viewer(renderer);
    renderer.fail_upload = true;
    viewer.open(sample);
    loop.settle(viewer);
    CHECK(viewer.failed());
    CHECK(renderer.textures.empty());
    viewer.open(sample);
    loop.settle(viewer);
    REQUIRE(viewer.has_image());
    CHECK_FALSE(viewer.failed());
    viewer.open(sample.parent_path() / "missing-image.jpg");
    loop.settle(viewer);
    CHECK(viewer.error() == LV_ERROR_IO);
    CHECK(viewer.has_image()); // A prior valid texture is retained on load failure.
    CHECK(renderer.textures.size() == 1);
    viewer.open(sample);
    loop.settle(viewer);
    CHECK_FALSE(viewer.failed());
    viewer.shutdown();
    CHECK(renderer.textures.empty());
}

TEST_CASE("viewer automatically displays later GIF frames and reuses two textures", "[viewer][animation]")
{
    EventLoop loop;
    TestRenderer renderer;
    liltview::Viewer viewer(renderer);
    viewer.open(std::filesystem::path(LILTVIEW_IMAGE_FIXTURES) / "red-blue-loop.gif");
    loop.settle(viewer);
    REQUIRE(viewer.has_image());
    CHECK_FALSE(viewer.failed());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (viewer.displayed_frame_index() != 1)
    {
        (viewer.pump(), viewer.prepare_loading_view());
        REQUIRE_FALSE(viewer.failed());
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        SDL_Event event;
        SDL_WaitEventTimeout(&event, viewer.wait_timeout_ms(10));
    }
    for (std::uint64_t index : {0u, 1u})
    {
        while (viewer.displayed_frame_index() != index)
        {
            (viewer.pump(), viewer.prepare_loading_view());
            REQUIRE_FALSE(viewer.failed());
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            SDL_Event event;
            SDL_WaitEventTimeout(&event, viewer.wait_timeout_ms(10));
        }
    }
    CHECK(renderer.creates == 2);
    viewer.open(sample);
    loop.settle(viewer);
    CHECK_FALSE(viewer.failed());
    CHECK(renderer.textures.size() == 1);
    viewer.shutdown();
    CHECK(renderer.textures.empty());
}

TEST_CASE("viewer freezes valid GIF display on upload failure and can open another file", "[viewer][animation]")
{
    EventLoop loop;
    TestRenderer renderer;
    liltview::Viewer viewer(renderer);
    viewer.open(std::filesystem::path(LILTVIEW_IMAGE_FIXTURES) / "red-first-frame.gif");
    loop.settle(viewer);
    REQUIRE(viewer.has_image());
    renderer.fail_upload = true;
    const auto deadline  = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!viewer.failed())
    {
        (viewer.pump(), viewer.prepare_loading_view());
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        SDL_Event event;
        SDL_WaitEventTimeout(&event, 10);
    }
    CHECK(viewer.has_image());
    viewer.open(sample);
    loop.settle(viewer);
    CHECK_FALSE(viewer.failed());
    CHECK(renderer.textures.size() == 1);
}

TEST_CASE("animation GPU budget failure keeps the last complete texture", "[viewer][animation][budget]")
{
    EventLoop loop;
    TestRenderer renderer;
    renderer.image_budget = 96 * 64 * 4 + 1;
    liltview::Viewer viewer(renderer);
    viewer.open(std::filesystem::path(LILTVIEW_IMAGE_FIXTURES) / "red-blue-loop.gif");
    loop.settle(viewer);
    REQUIRE(viewer.has_image());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!viewer.failed())
    {
        (viewer.pump(), viewer.prepare_loading_view());
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        SDL_Event event;
        SDL_WaitEventTimeout(&event, 10);
    }
    CHECK(viewer.error() == LV_ERROR_MEMORY_BUDGET);
    CHECK(viewer.has_image());
    CHECK(renderer.creates == 1);
    CHECK(renderer.textures.size() == 1);
}

TEST_CASE("native scenes retain every decoded tile across camera changes", "[viewer][native]")
{
    EventLoop loop;
    TestRenderer renderer;
    renderer.max_edge = 128;
    liltview::Viewer viewer(renderer);
    const auto folder =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("liltview-native-cache-" + std::to_string(SDL_GetPerformanceCounter()));
    std::filesystem::create_directories(folder);
    const auto path = folder / "board.png";
    liltview::test::checkerboard(path, 512, 512, 1);
    viewer.open(path);
    loop.settle(viewer);
    REQUIRE(viewer.has_image());
    REQUIRE(viewer.detail_ready());
    REQUIRE(renderer.textures.size() == 16); // Native tiles, no FIT texture.
    const auto uploads = renderer.uploads;
    const auto creates = renderer.creates;
    std::filesystem::remove(path); // There is no source left to decode again.
    viewer.set_viewport({0, 0}, {128, 96});
    for (unsigned i = 0; i < 12; ++i)
    {
        viewer.actual_size();
        viewer.pan(-20, 10);
        viewer.rotate(i % 2 == 0 ? -1 : 1);
        if (i % 2 == 0)
            viewer.zoom(-2); // Keyboard/menu zoom shares the decode-free camera path.
        else
            viewer.zoom(-2, {64, 48});
        viewer.fit();
        loop.settle(viewer);
        CHECK_FALSE(viewer.failed());
        CHECK(viewer.detail_ready());
    }
    CHECK(renderer.uploads == uploads);
    CHECK(renderer.creates == creates);
    viewer.shutdown();
    CHECK(renderer.textures.empty());
    std::filesystem::remove_all(folder);
}

TEST_CASE("cached native scene remains an immutable snapshot after source replacement", "[viewer][native][source]")
{
    EventLoop loop;
    TestRenderer renderer;
    liltview::Viewer viewer(renderer);
    const auto folder =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("liltview-native-snapshot-" + std::to_string(SDL_GetPerformanceCounter()));
    std::filesystem::create_directories(folder);
    const auto path = folder / "board.png";
    liltview::test::checkerboard(path, 513, 513, 17);
    viewer.open(path);
    loop.settle(viewer);
    REQUIRE(viewer.detail_ready());
    const auto uploads = renderer.uploads;
    liltview::test::checkerboard(path, 1024, 1024, 23);
    viewer.set_viewport({0, 0}, {128, 96});
    viewer.actual_size();
    loop.settle(viewer);
    CHECK_FALSE(viewer.failed());
    CHECK(viewer.source_size().width == 513);
    CHECK(renderer.uploads == uploads);
    viewer.shutdown();
    std::filesystem::remove_all(folder);
}

TEST_CASE("failed replacement preserves the displayed source for sharp regional zoom", "[viewer][region][source]")
{
    EventLoop loop;
    TestRenderer renderer;
    liltview::Viewer viewer(renderer);
    auto folder = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
        ("liltview-viewer-failed-replacement-" + std::to_string(SDL_GetPerformanceCounter()));
    std::filesystem::create_directories(folder);
    const auto path = folder / "board.png";
    liltview::test::checkerboard(path, 1024, 1024, 17);
    viewer.open(path);
    loop.settle(viewer);
    REQUIRE(viewer.has_image());
    viewer.open(folder / "missing.png");
    loop.settle(viewer);
    CHECK(viewer.error() == LV_ERROR_IO);
    REQUIRE(viewer.has_image());
    viewer.set_viewport({0, 0}, {128, 96});
    viewer.actual_size();
    loop.settle(viewer);
    CHECK_FALSE(viewer.failed());
    CHECK(viewer.detail_ready());
    viewer.shutdown();
    std::filesystem::remove_all(folder);
}

TEST_CASE("a native-size preview uses pixel-preserving magnification", "[viewer][sharpness]")
{
    EventLoop loop;
    TestRenderer renderer;
    const auto folder =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("liltview-native-preview-" + std::to_string(SDL_GetPerformanceCounter()));
    std::filesystem::create_directories(folder);
    const auto path = folder / "small.png";
    liltview::test::checkerboard(path, 17, 13, 1);
    liltview::Viewer viewer(renderer);
    viewer.open(path);
    loop.settle(viewer);
    REQUIRE(viewer.has_image());
    REQUIRE(renderer.textures.size() == 1);
    CHECK(renderer.textures.begin()->second.magnification == liltview::render::ImageDesc::Magnification::nearest);
    viewer.shutdown();
    std::filesystem::remove_all(folder);
}

TEST_CASE("native uploads overlap decoding but the first visible image is complete", "[viewer][native][stream]")
{
    EventLoop loop;
    TestRenderer renderer;
    renderer.upload_delay_ms = 5; // Stop the UI pump after the first uploaded tile.
    const auto folder =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("liltview-native-first-band-" + std::to_string(SDL_GetPerformanceCounter()));
    std::filesystem::create_directories(folder);
    const auto path = folder / "stream.png";
    liltview::test::checkerboard(path, 1024, 4096, 7);
    liltview::Viewer viewer(renderer);
    viewer.open(path);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (viewer.static_load_stats().tiles == 0)
    {
        (viewer.pump(), viewer.prepare_loading_view());
        REQUIRE_FALSE(viewer.failed());
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        SDL_Delay(1);
    }
    CHECK(viewer.loading());
    CHECK_FALSE(viewer.has_visible_image());
    CHECK_FALSE(viewer.settled());
    CHECK(viewer.source_size().width == 1024);
    CHECK(viewer.source_size().height == 4096);
    renderer.upload_delay_ms = 0;
    loop.settle(viewer);
    CHECK(viewer.detail_ready());
    CHECK(viewer.has_visible_image());
    CHECK(viewer.static_load_stats().first_upload_ms < viewer.static_load_stats().decode_done_ms);
    viewer.shutdown();
    std::filesystem::remove_all(folder);
}

namespace
{
struct DetailsDirectory
{
    std::filesystem::path path =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("liltview-information-" + std::to_string(SDL_GetPerformanceCounter()));
    DetailsDirectory()
    {
        std::filesystem::create_directories(path);
    }
    ~DetailsDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

struct InformationGui
{
    InformationGui()
    {
        ImGui::CreateContext();
        auto &io       = ImGui::GetIO();
        io.DisplaySize = {800, 600};
        io.DeltaTime   = 1.0f / 60;
        io.IniFilename = io.LogFilename = nullptr;
        io.Fonts->AddFontDefault();
        unsigned char *pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.Fonts->SetTexID(1);
    }
    ~InformationGui()
    {
        ImGui::DestroyContext();
    }
    void begin_frame(liltview::Viewer &viewer, float density)
    {
        ImGui::GetIO().DisplayFramebufferScale = {density, density};
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({800, 600});
        ImGui::Begin("information_camera", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        viewer.draw({0, 0}, {320, 240}, density, false);
        ImGui::End();
    }
    void frame(liltview::Viewer &viewer, float density)
    {
        begin_frame(viewer, density);
        ImGui::Render();
    }
};

// Real decoder/navigation with a deferred test backend. Delaying one upload
// exhausts pump's 4 ms budget, making a partial scene deterministic on MSVC too.
struct ProgressiveNavigation
{
    EventLoop loop;
    TestRenderer renderer;
    InformationGui gui;
    DetailsDirectory directory;
    liltview::Viewer viewer{renderer};
    std::filesystem::path target;

    ProgressiveNavigation(const char *extension, bool cached, int direction)
    {
        renderer.image_budget        = (cached ? 4ULL : 128ULL) * 1024 * 1024;
        renderer.strict_image_budget = true;
        renderer.record_draws        = true;
        const auto first             = directory.path / (std::string("1.") + extension);
        const auto middle            = directory.path / (std::string("2.") + extension);
        const auto last              = directory.path / (std::string("3.") + extension);
        liltview::test::checkerboard(first, 64, 64, 7);
        liltview::test::checkerboard(middle, 1024, 4096, 7);
        liltview::test::checkerboard(last, 64, 64, 7);
        target = direction > 0 ? last : first;
        viewer.open(direction > 0 ? first : last);
        loop.settle_prefetch(viewer);
        REQUIRE_FALSE(viewer.failed());
        renderer.upload_delay_ms = 5;
        viewer.navigate(direction);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;)
        {
            (viewer.pump(), viewer.prepare_loading_view());
            REQUIRE_FALSE(viewer.failed());
            if (viewer.loading() && viewer.static_load_stats().tiles > 0)
            {
                REQUIRE_FALSE(viewer.has_visible_image());
                renderer.upload_delay_ms = 0;
                loop.settle(viewer);
                break;
            }
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            SDL_Delay(1);
        }
    }

    std::vector<TestRenderer::Image> draw()
    {
        renderer.begin_frame();
        gui.begin_frame(viewer, 1);
        if (!viewer.has_visible_image()) CHECK(renderer.frame_images.empty());
        else REQUIRE_FALSE(renderer.frame_images.empty());
        return renderer.frame_images;
    }
    void submit()
    {
        ImGui::Render();
        renderer.render_frame(ImGui::GetDrawData());
        renderer.present();
    }
    void check_images(const std::vector<TestRenderer::Image> &images, bool live)
    {
        for (const auto image : images)
        {
            INFO("Progressive image slot: " << image.slot);
            CHECK(renderer.textures.contains(image.slot) == live);
        }
    }
    void finish()
    {
        renderer.upload_delay_ms = 0;
        loop.settle_prefetch(viewer);
        REQUIRE_FALSE(viewer.failed());
        REQUIRE(viewer.image_details());
        CHECK(viewer.image_details()->path == target);
        CHECK(renderer.capabilities().image_bytes == liltview::render::image_storage_bytes({{64, 64}, true}));
        CHECK(renderer.budget_rejections == 0);
        viewer.shutdown();
        CHECK(renderer.textures.empty());
    }
};
} // namespace

TEST_CASE("canvas navigation retains progressive textures until frame submission", "[viewer][navigation][submission]")
{
    for (const auto *extension : {"png", "jpg"})
        for (const bool cached : {false, true})
            for (const int direction : {1, -1})
            {
                CAPTURE(extension, cached, direction);
                ProgressiveNavigation test(extension, cached, direction);
                const auto images = test.draw();
                // Shell builds draw commands before Application dispatches the action.
                // Multiple actions must not swap the retired scene back into deletion.
                for (const int step : {direction, -direction, direction})
                {
                    test.viewer.navigate(step);
                    test.check_images(images, true);
                }
                test.submit(); // The backend must still resolve every queued texture.
                test.check_images(images, true);
                test.renderer.upload_delay_ms = 0;
                (test.viewer.pump(), test.viewer.prepare_loading_view());
                test.check_images(images, true); // Old completed image is retained until replacement commits.
                test.finish();
            }
}

TEST_CASE("keyboard navigation releases retired progressive textures before the next frame", "[viewer][navigation][submission]")
{
    for (const auto *extension : {"png", "jpg"})
        for (const bool cached : {false, true})
            for (const int direction : {1, -1})
            {
                CAPTURE(extension, cached, direction);
                ProgressiveNavigation test(extension, cached, direction);
                const auto images = test.draw();
                test.submit();
                // SDL keyboard events navigate between frames, before the next pump.
                test.viewer.navigate(direction);
                test.renderer.upload_delay_ms = 0;
                (test.viewer.pump(), test.viewer.prepare_loading_view());
                test.check_images(images, true);
                test.draw();
                test.submit();
                test.finish();
            }
}

TEST_CASE("shutdown releases retired progressive textures without another pump", "[viewer][navigation][submission][shutdown]")
{
    for (const auto *extension : {"png", "jpg"})
        for (const bool cached : {false, true})
        {
            CAPTURE(extension, cached);
            ProgressiveNavigation test(extension, cached, 1);
            const auto images = test.draw();
            test.viewer.navigate(1);
            test.check_images(images, true);
            test.submit();
            test.viewer.shutdown();
            CHECK(test.renderer.textures.empty());
            CHECK(test.renderer.capabilities().image_bytes == 0);
            test.viewer.shutdown(); // Cleanup remains idempotent.
        }
}

TEST_CASE("rapid bidirectional navigation survives cancelled loads and neighbor prefetch", "[viewer][navigation][rapid]")
{
    EventLoop loop;
    TestRenderer renderer;
    InformationGui gui;
    const auto folder = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
        ("liltview-rapid-navigation-" + std::to_string(SDL_GetPerformanceCounter()));
    std::filesystem::create_directories(folder);
    for (unsigned i = 0; i < 12; ++i)
        liltview::test::checkerboard(folder / (std::to_string(i) + ".png"), i % 2 == 0 ? 2048u : 31u, i % 2 == 0 ? 1536u : 23u,
                                     7);
    liltview::Viewer viewer(renderer);
    viewer.open(folder / "0.png");
    loop.settle_prefetch(viewer);
    for (unsigned i = 0; i < 1000; ++i)
    {
        const auto &navigation = viewer.navigation();
        const int direction    = (i / 11) % 2 == 0 ? 1 : -1;
        REQUIRE((direction == 1 ? navigation.can_next : navigation.can_previous));
        viewer.navigate(direction);
        (viewer.pump(), viewer.prepare_loading_view());
        gui.frame(viewer, 1);
        if (i % 3 == 0)
            SDL_Delay(1);
    }
    loop.settle_prefetch(viewer);
    CHECK_FALSE(viewer.failed());
    REQUIRE(viewer.image_details());
    CHECK(viewer.image_details()->path == viewer.navigation().selected_path);
    viewer.shutdown();
    CHECK(renderer.textures.empty());
    std::filesystem::remove_all(folder);
}

TEST_CASE("image details switch with complete first view and retain prior source after replacement failure",
          "[viewer][image_details]")
{
    EventLoop loop;
    DetailsDirectory directory;
    TestRenderer renderer;
    const auto first  = directory.path / "原图-%s-100%.png";
    const auto second = directory.path / "stream.png";
    const auto third  = directory.path / "last.png";
    liltview::test::checkerboard(first, 17, 13, 1);
    liltview::test::checkerboard(second, 1024, 4096, 7);
    liltview::test::checkerboard(third, 31, 29, 1);
    liltview::Viewer viewer(renderer);
    CHECK_FALSE(viewer.image_details());
    viewer.open(first);
    loop.settle(viewer);
    auto details = viewer.image_details();
    REQUIRE(details);
    CHECK(details->path == first);
    CHECK(details->image.file_bytes == std::filesystem::file_size(first));
    CHECK(details->image.flags & LV_INFO_FILE_BYTES_KNOWN);
    CHECK(details->pixel_format == LV_PIXEL_RGBA8);
    CHECK(details->color_space == LV_COLOR_UNKNOWN);
    CHECK_FALSE(details->animation);

    renderer.upload_delay_ms = 5; // One upload exhausts the UI pump's 4 ms window.
    viewer.open(second);
    details = viewer.image_details();
    REQUIRE(details);
    CHECK(details->path == first);
    CHECK(details->loading_replacement);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (viewer.static_load_stats().tiles == 0)
    {
        (viewer.pump(), viewer.prepare_loading_view());
        REQUIRE_FALSE(viewer.failed());
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        SDL_Delay(1);
    }
    details = viewer.image_details();
    REQUIRE(details);
    CHECK(details->path == first); // Metadata switches atomically with the first complete image.
    CHECK(details->image.source_size.width == 17);
    CHECK(details->image.source_size.height == 13);
    CHECK(details->pixel_format == LV_PIXEL_RGBA8);
    CHECK_FALSE(details->partial);
    CHECK_FALSE(viewer.has_visible_image());
    renderer.fail_upload = true;
    loop.settle(viewer);
    REQUIRE(viewer.failed());
    details = viewer.image_details();
    REQUIRE(details);
    CHECK(details->path == first);
    CHECK(details->image.source_size.width == 17);
    CHECK(details->image.source_size.height == 13);
    CHECK_FALSE(details->partial);

    renderer.upload_delay_ms = 0;
    viewer.open(second);
    loop.settle(viewer);
    details = viewer.image_details();
    REQUIRE(details);
    CHECK(details->path == second);
    CHECK_FALSE(details->partial);
    viewer.open(first);
    (viewer.pump(), viewer.prepare_loading_view());
    viewer.open(second);
    viewer.open(third);
    loop.settle(viewer);
    details = viewer.image_details();
    REQUIRE(details);
    CHECK(details->path == third);
    CHECK(details->image.source_size.width == 31);
    CHECK(details->image.source_size.height == 29);
    const auto uploads = renderer.uploads;
    std::filesystem::remove(third);
    for (unsigned count = 0; count < 20; ++count)
    {
        const auto snapshot = viewer.image_details();
        REQUIRE(snapshot);
        CHECK(snapshot->path == third);
        CHECK(snapshot->image.file_bytes == details->image.file_bytes);
    }
    CHECK(renderer.uploads == uploads);
    CHECK_FALSE(viewer.failed());
    viewer.shutdown();
    CHECK_FALSE(viewer.image_details());
}

TEST_CASE("image details remain empty after an initial failed open", "[viewer][image_details]")
{
    EventLoop loop;
    DetailsDirectory directory;
    TestRenderer renderer;
    liltview::Viewer viewer(renderer);
    viewer.open(directory.path / "missing.png");
    loop.settle(viewer);
    CHECK(viewer.failed());
    CHECK_FALSE(viewer.image_details());
}

TEST_CASE("resident scene rotation draws transformed quads without decoding allocating or uploading pixels",
          "[viewer][rotation][native]")
{
    EventLoop loop;
    DetailsDirectory directory;
    TestRenderer renderer;
    renderer.max_edge = 128;
    const auto file   = directory.path / "board.png";
    liltview::test::checkerboard(file, 512, 384, 17);
    liltview::Viewer viewer(renderer);
    viewer.open(file);
    loop.settle(viewer);
    InformationGui gui;
    gui.frame(viewer, 1);
    const auto original       = viewer.presented_view();
    const auto original_reads = viewer.last_source_bytes_read();
    const auto uploads        = renderer.uploads;
    const auto creates        = renderer.creates;
    const auto textures       = renderer.textures.size();
    std::filesystem::remove(file);
    for (unsigned turn = 0; turn < 4; ++turn)
    {
        viewer.rotate(1);
        gui.frame(viewer, 1);
        CHECK_FALSE(viewer.failed());
        CHECK_FALSE(viewer.detail_loading());
        CHECK(renderer.uploads == uploads);
        CHECK(renderer.creates == creates);
        CHECK(renderer.textures.size() == textures);
        CHECK(viewer.last_source_bytes_read() == original_reads);
        if (turn < 3)
            CHECK_FALSE(viewer.presented_view().same_view(original));
    }
    CHECK(viewer.presented_view().same_view(original));
    viewer.rotate(-1);
    gui.frame(viewer, 1);
    CHECK_FALSE(viewer.presented_view().same_view(original));
    // Opening another image resets manual rotation even for the same dimensions.
    const auto next = directory.path / "next.png";
    liltview::test::checkerboard(next, 512, 384, 17);
    viewer.open(next);
    loop.settle(viewer);
    gui.frame(viewer, 1);
    CHECK(viewer.presented_view().same_view(original));
    viewer.shutdown();
    CHECK(renderer.textures.empty());
}

TEST_CASE("image details report physical displayed zoom while a cold target is still restoring",
          "[viewer][image_details][camera]")
{
    EventLoop loop;
    DetailsDirectory directory;
    TestRenderer renderer;
    renderer.image_budget = 8 * 1024 * 1024;
    const auto file       = directory.path / "large.png";
    liltview::test::checkerboard(file, 2048, 2048, 31);
    liltview::Viewer viewer(renderer);
    viewer.set_viewport({0,0}, {320,240}, 2); // Supply the actual initial canvas before first-view preparation.
    viewer.open(file);
    loop.settle(viewer);
    InformationGui gui;
    gui.frame(viewer, 2);
    viewer.actual_size();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        (viewer.pump(), viewer.prepare_loading_view());
        gui.frame(viewer, 2);
        REQUIRE_FALSE(viewer.failed());
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        SDL_Delay(1);
    } while (viewer.detail_loading());
    auto details = viewer.image_details();
    REQUIRE(details);
    CHECK(details->scale == Catch::Approx(1)); // Density 2 is already included in Viewport.scale().
    renderer.allow_upload = false;
    viewer.zoom(-3);
    gui.frame(viewer, 2);
    REQUIRE(viewer.detail_loading());
    CHECK_FALSE(viewer.interaction_ready());
    CHECK(viewer.image_loaded());
    details = viewer.image_details();
    REQUIRE(details);
    CHECK(details->scale == viewer.presented_view().scale());
    CHECK(details->scale > 0.512);
    renderer.allow_upload = true;
}

TEST_CASE("image details preserve final GIF counts and the displayed frame after the session closes",
          "[viewer][image_details][animation]")
{
    EventLoop loop;
    DetailsDirectory directory;
    const auto source = std::filesystem::path(LILTVIEW_IMAGE_FIXTURES) / "red-blue-loop.gif";
    std::ifstream input(source, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto loop_extension = bytes.find("NETSCAPE2.0");
    REQUIRE(loop_extension != std::string::npos);
    REQUIRE(loop_extension >= 3);
    bytes.erase(loop_extension - 3, 19); // No loop extension means one finite pass.
    const auto file = directory.path / "finite.gif";
    {
        std::ofstream output(file, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    TestRenderer renderer;
    liltview::Viewer viewer(renderer);
    viewer.open(file);
    loop.settle(viewer);
    auto details = viewer.image_details();
    REQUIRE(details);
    REQUIRE(details->animation);
    CHECK(details->path == file);
    CHECK(details->image.format == LV_FORMAT_GIF);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (viewer.animating())
    {
        (viewer.pump(), viewer.prepare_loading_view());
        REQUIRE_FALSE(viewer.failed());
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        SDL_Delay(1);
    }
    details = viewer.image_details();
    REQUIRE(details);
    REQUIRE(details->animation);
    CHECK(details->animation->info.flags & LV_ANIMATION_FRAME_COUNT_KNOWN);
    CHECK(details->animation->info.flags & LV_ANIMATION_DURATION_KNOWN);
    CHECK(details->animation->info.flags & LV_ANIMATION_PLAY_COUNT_KNOWN);
    CHECK(details->animation->info.frame_count == 2);
    CHECK(details->animation->info.total_plays == 1);
    CHECK(details->animation->frame.key.frame_index == 1);
    CHECK(details->pixel_format == LV_PIXEL_RGBA8);
    viewer.open(directory.path / "missing.png");
    loop.settle(viewer);
    details = viewer.image_details();
    REQUIRE(details);
    CHECK(details->path == file);
    REQUIRE(details->animation);
    CHECK(details->animation->info.total_plays == 1);
}

namespace
{
struct WarmDirectory
{
    std::filesystem::path path =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("liltview-warm-viewer-" + std::to_string(SDL_GetPerformanceCounter()));
    WarmDirectory()
    {
        std::filesystem::create_directories(path);
    }
    ~WarmDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void write_warm_gif(const std::filesystem::path &path, unsigned frames = 2, unsigned width = 4, unsigned height = 3,
                    unsigned total_plays = 1, unsigned delay_cs = 8)
{
    struct Encoder
    {
        GifFileType *file       = nullptr;
        ColorMapObject *palette = nullptr;
        ~Encoder()
        {
            int ignored = 0;
            if (file)
                EGifCloseFile(file, &ignored);
            if (palette)
                GifFreeMapObject(palette);
        }
    } encoder;
    int error    = 0;
    encoder.file = EGifOpenFileName(path.string().c_str(), false, &error);
    REQUIRE(encoder.file);
    EGifSetGifVersion(encoder.file, true);
    const GifColorType colors[]{{255, 0, 0}, {0, 0, 255}};
    encoder.palette = GifMakeMapObject(2, colors);
    REQUIRE(encoder.palette);
    REQUIRE(EGifPutScreenDesc(encoder.file, static_cast<int>(width), static_cast<int>(height), 8, 0, encoder.palette) == GIF_OK);
    if (total_plays != 1)
    {
        const auto repeat = total_plays ? total_plays - 1 : 0;
        const GifByteType loop[]{1, static_cast<GifByteType>(repeat), static_cast<GifByteType>(repeat >> 8)};
        REQUIRE(EGifPutExtensionLeader(encoder.file, APPLICATION_EXT_FUNC_CODE) == GIF_OK);
        REQUIRE(EGifPutExtensionBlock(encoder.file, 11, "NETSCAPE2.0") == GIF_OK);
        REQUIRE(EGifPutExtensionBlock(encoder.file, 3, loop) == GIF_OK);
        REQUIRE(EGifPutExtensionTrailer(encoder.file) == GIF_OK);
    }
    for (unsigned frame = 0; frame < frames; ++frame)
    {
        GraphicsControlBlock control{};
        control.DisposalMode     = DISPOSE_DO_NOT;
        control.DelayTime        = static_cast<int>(delay_cs);
        control.TransparentColor = NO_TRANSPARENT_COLOR;
        GifByteType extension[4];
        REQUIRE(EGifGCBToExtension(&control, extension) == 4);
        REQUIRE(EGifPutExtension(encoder.file, GRAPHICS_EXT_FUNC_CODE, 4, extension) == GIF_OK);
        REQUIRE(EGifPutImageDesc(encoder.file, 0, 0, static_cast<int>(width), static_cast<int>(height), false, nullptr) ==
                GIF_OK);
        std::vector<GifPixelType> row(width, static_cast<GifPixelType>(frame % 2));
        for (unsigned y = 0; y < height; ++y)
            REQUIRE(EGifPutLine(encoder.file, row.data(), static_cast<int>(width)) == GIF_OK);
    }
    auto *file   = encoder.file;
    encoder.file = nullptr;
    REQUIRE(EGifCloseFile(file, &error) == GIF_OK);
}

void check_background_quota(const liltview::Viewer &viewer)
{
    CHECK(viewer.prefetch_stats().active + viewer.animation_warmup_stats().outstanding_requests <= 2);
}

template <class Predicate>
void warm_until(liltview::Viewer &viewer, Predicate done, bool allow_failure = false,
                std::chrono::seconds timeout = std::chrono::seconds(8))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done())
    {
        (viewer.pump(), viewer.prepare_loading_view());
        check_background_quota(viewer);
        if (!allow_failure)
            REQUIRE_FALSE(viewer.failed());
        if (done())
            return;
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        SDL_Event event;
        SDL_WaitEventTimeout(&event, viewer.wait_timeout_ms(2));
    }
}

void ready_neighbors(liltview::Viewer &viewer, std::size_t count = 1, std::chrono::seconds timeout = std::chrono::seconds(8))
{
    warm_until(
        viewer, [&]
        { return viewer.animation_warmup_stats().ready == count && !viewer.prefetch_pending() && !viewer.navigation().scanning; },
        false, timeout);
}

struct WarmGui
{
    WarmGui()
    {
        ImGui::CreateContext();
        auto &io       = ImGui::GetIO();
        io.DisplaySize = {128, 96};
        io.DeltaTime   = 1.0f / 60;
        io.IniFilename = io.LogFilename = nullptr;
        io.Fonts->AddFontDefault();
        unsigned char *pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.Fonts->SetTexID(1);
    }
    ~WarmGui()
    {
        ImGui::DestroyContext();
    }
    void draw(liltview::Viewer &viewer, TestRenderer &renderer)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({128, 96});
        ImGui::Begin("warm_frame", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        renderer.last_drawn_pixel.reset();
        viewer.draw({0, 0}, {128, 96}, 1, false);
        ImGui::End();
        ImGui::Render();
        REQUIRE(renderer.last_drawn_pixel);
    }
};

void open_static_for_warm(EventLoop &loop, liltview::Viewer &viewer, const std::filesystem::path &folder)
{
    liltview::test::checkerboard(folder / "1.bmp", 31, 23, 7);
    viewer.open(folder / "1.bmp");
    loop.settle(viewer);
}
} // namespace

TEST_CASE("GIF neighbor warms by content without GPU work and presents genuine frames zero and one", "[viewer][warm]")
{
    EventLoop loop;
    WarmDirectory directory;
    write_warm_gif(directory.path / "2.png"); // Encoded GIF despite its PNG suffix.
    TestRenderer renderer;
    liltview::Viewer viewer(renderer);
    open_static_for_warm(loop, viewer, directory.path);
    const auto uploads = renderer.uploads, creates = renderer.creates;
    const auto textures = renderer.textures.size();
    ready_neighbors(viewer);
    CHECK(renderer.uploads == uploads);
    CHECK(renderer.creates == creates);
    CHECK(renderer.textures.size() == textures);
    CHECK(viewer.animation_frames_presented() == 0);
    CHECK(viewer.animation_warmup_stats().retained_bytes > 0);
    viewer.navigate(1);
    warm_until(viewer, [&] { return viewer.warmed_first_frames_presented() == 1; });
    CHECK(viewer.displayed_frame_index() == 0);
    CHECK(viewer.animation_frames_presented() == 1);
    REQUIRE(viewer.image_details());
    REQUIRE(viewer.image_details()->animation);
    CHECK(viewer.image_details()->path == directory.path / "2.png");
    CHECK(viewer.image_details()->image.format == LV_FORMAT_GIF);
    WarmGui gui;
    gui.draw(viewer, renderer);
    CHECK(renderer.last_drawn_pixel == std::array<std::uint8_t, 4>{255, 0, 0, 255});
    viewer.suspend_animation(true);
    for (unsigned i = 0; i < 100; ++i)
    {
        (viewer.pump(), viewer.prepare_loading_view());
        CHECK(viewer.animation_frames_presented() == 1);
        CHECK(viewer.warmed_first_frames_presented() == 1);
    }
    viewer.suspend_animation(false);
    warm_until(viewer, [&] { return viewer.animation_frames_presented() == 2; });
    CHECK(viewer.displayed_frame_index() == 1);
    CHECK(viewer.warmed_first_frames_presented() == 1);
    gui.draw(viewer, renderer);
    CHECK(renderer.last_drawn_pixel == std::array<std::uint8_t, 4>{0, 0, 255, 255});
    CHECK(viewer.animation_warmup_stats().adopted == 1);
    viewer.shutdown();
    CHECK(renderer.textures.empty());
}

TEST_CASE("ready GIF palette rewrites and deleted sources never upload stale warmed frame zero", "[viewer][warm][source]")
{
    for (const bool erase : {false, true})
    {
        EventLoop loop;
        WarmDirectory directory;
        const auto file = directory.path / "2.png";
        write_warm_gif(file);
        TestRenderer renderer;
        liltview::Viewer viewer(renderer);
        open_static_for_warm(loop, viewer, directory.path);
        ready_neighbors(viewer);
        const auto upload_count = renderer.uploaded_first_pixels.size();
        if (erase)
            std::filesystem::remove(file);
        else
        {
            const auto length = std::filesystem::file_size(file);
            const auto time   = std::filesystem::last_write_time(file);
            {
                std::fstream output(file, std::ios::in | std::ios::out | std::ios::binary);
                REQUIRE(output.good());
                output.seekp(13); // First global color entry is RGB red; rewrite it to blue without changing length.
                const std::array<char, 3> blue{0, 0, static_cast<char>(255)};
                output.write(blue.data(), static_cast<std::streamsize>(blue.size()));
                output.flush();
                REQUIRE(output.good());
            }
            std::filesystem::last_write_time(file, time + std::chrono::seconds(1));
            CHECK(std::filesystem::file_size(file) == length);
        }
        viewer.navigate(1);
        warm_until(viewer, [&] { return !viewer.loading(); }, erase);
        CHECK(viewer.warmed_first_frames_presented() == 0);
        REQUIRE(viewer.image_details());
        if (erase)
        {
            CHECK(viewer.error() == LV_ERROR_IO);
            CHECK(viewer.image_details()->path == directory.path / "1.bmp");
            CHECK(renderer.uploaded_first_pixels.size() == upload_count);
        }
        else
        {
            CHECK_FALSE(viewer.failed());
            CHECK(viewer.image_details()->path == file);
            REQUIRE(renderer.uploaded_first_pixels.size() > upload_count);
            for (std::size_t i = upload_count; i < renderer.uploaded_first_pixels.size(); ++i)
                CHECK(renderer.uploaded_first_pixels[i] == std::array<std::uint8_t, 4>{0, 0, 255, 255});
            CHECK(viewer.animation_frames_presented() == 1);
        }
        WarmGui gui;
        gui.draw(viewer, renderer);
        CHECK(renderer.last_drawn_pixel ==
              (erase ? std::array<std::uint8_t, 4>{255, 0, 0, 255} : std::array<std::uint8_t, 4>{0, 0, 255, 255}));
        viewer.shutdown();
        CHECK(renderer.textures.empty());
    }
}

TEST_CASE("changed framebuffer bounds bypass incompatible warmed animation pixels", "[viewer][warm][bounds]")
{
    EventLoop loop;
    WarmDirectory directory;
    write_warm_gif(directory.path / "2.png", 2, 64, 48);
    TestRenderer renderer;
    renderer.frame_size = {32, 24};
    liltview::Viewer viewer(renderer);
    open_static_for_warm(loop, viewer, directory.path);
    ready_neighbors(viewer);
    renderer.frame_size = {16, 12};
    viewer.navigate(1);
    warm_until(viewer, [&] { return !viewer.loading(); });
    CHECK(viewer.warmed_first_frames_presented() == 0);
    CHECK(viewer.animation_warmup_stats().adopted == 0);
    REQUIRE(viewer.image_details());
    CHECK(viewer.image_details()->image.canvas.width == 16);
    CHECK(viewer.image_details()->image.canvas.height == 12);
    CHECK(viewer.animation_frames_presented() == 1);
    viewer.shutdown();
}

TEST_CASE("warmed single-frame GIFs preserve finite and infinite source loop counts", "[viewer][warm][loop]")
{
    for (const unsigned plays : {1u, 3u, 0u})
    {
        CAPTURE(plays);
        EventLoop loop;
        WarmDirectory directory;
        write_warm_gif(directory.path / "2.png", 1, 4, 3, plays, 4);
        TestRenderer renderer;
        liltview::Viewer viewer(renderer);
        open_static_for_warm(loop, viewer, directory.path);
        ready_neighbors(viewer);
        viewer.navigate(1);
        warm_until(viewer,
                   [&]
                   {
                       const auto details = viewer.image_details();
                       return plays ? viewer.animation_frames_presented() == plays && !viewer.animating() :
                                      details && details->animation && details->animation->frame.key.epoch >= 3;
                   });
        CHECK(viewer.warmed_first_frames_presented() == 1);
        CHECK(viewer.displayed_frame_index() == 0);
        REQUIRE(viewer.image_details());
        REQUIRE(viewer.image_details()->animation);
        CHECK(viewer.image_details()->animation->info.total_plays == plays);
        CHECK(viewer.image_details()->animation->info.flags & LV_ANIMATION_PLAY_COUNT_KNOWN);
        if (plays)
            CHECK(viewer.animation_frames_presented() == plays);
        else
        {
            CHECK(viewer.animation_frames_presented() >= 3);
            CHECK(viewer.animating());
        }
        viewer.shutdown();
        CHECK(renderer.textures.empty());
    }
}

TEST_CASE("paused verified GIF adoption can be replaced or shut down before presentation", "[viewer][warm][shutdown]")
{
    for (const bool replace : {false, true})
    {
        EventLoop loop;
        WarmDirectory directory;
        write_warm_gif(directory.path / "2.png");
        liltview::test::checkerboard(directory.path / "3.bmp", 17, 13, 7);
        TestRenderer renderer;
        liltview::Viewer viewer(renderer);
        open_static_for_warm(loop, viewer, directory.path);
        ready_neighbors(viewer);
        const auto uploads = renderer.uploads;
        viewer.suspend_animation(true);
        viewer.navigate(1);
        warm_until(viewer, [&]
                   { return viewer.animation_warmup_stats().adopted == 1 && !viewer.animation_warmup_stats().adoption_pending; });
        CHECK(viewer.loading());
        CHECK(viewer.warmed_first_frames_presented() == 0);
        CHECK(viewer.animation_frames_presented() == 0);
        CHECK(renderer.uploads == uploads);
        REQUIRE(viewer.image_details());
        CHECK(viewer.image_details()->path == directory.path / "1.bmp");
        if (replace)
        {
            viewer.open(directory.path / "3.bmp");
            loop.settle(viewer);
            REQUIRE_FALSE(viewer.failed());
            CHECK(viewer.image_details()->path == directory.path / "3.bmp");
            CHECK_FALSE(viewer.image_details()->animation);
            CHECK(viewer.warmed_first_frames_presented() == 0);
            CHECK(viewer.animation_frames_presented() == 0);
        }
        viewer.begin_shutdown();
        REQUIRE(viewer.join_shutdown(5000));
        viewer.shutdown();
        CHECK(renderer.textures.empty());
        CHECK(viewer.animation_warmup_stats().retained_bytes == 0);
        CHECK_FALSE(viewer.prefetch_pending());
    }
}

TEST_CASE("foreground GIF advances while neighbor warmups and errors stay within background quota", "[viewer][warm][priority]")
{
    EventLoop loop;
    WarmDirectory directory;
    write_warm_gif(directory.path / "1.png");
    write_warm_gif(directory.path / "2.gif", 2, 4, 3, 0);
    std::ofstream(directory.path / "3.png") << "damaged image";
    TestRenderer renderer;
    liltview::Viewer viewer(renderer);
    viewer.open(directory.path / "2.gif");
    warm_until(viewer,
               [&]
               {
                   return viewer.animation_frames_presented() >= 3 && viewer.animation_warmup_stats().ready == 1 &&
                       viewer.prefetch_stats().failed >= 1 && viewer.animation_warmup_stats().failed >= 1;
               });
    CHECK_FALSE(viewer.failed());
    REQUIRE(viewer.image_details());
    REQUIRE(viewer.image_details()->animation);
    CHECK(viewer.image_details()->path == directory.path / "2.gif");
    const auto animation = *viewer.image_details()->animation;
    CHECK(animation.info.animation_id == animation.frame.key.animation_id);
    CHECK(animation.frame.key.frame_index <= 1);
    CHECK(viewer.warmed_first_frames_presented() == 0);
    CHECK(viewer.animation_warmup_stats().ready == 1);
    viewer.shutdown();
}

TEST_CASE("unrelated foreground GIF admission releases two large READY resident canvases", "[viewer][warm][budget]")
{
    EventLoop loop;
    WarmDirectory directory;
    write_warm_gif(directory.path / "1.png", 2, 2500, 2500);
    write_warm_gif(directory.path / "3.png", 2, 2500, 2500);
    std::filesystem::create_directory(directory.path / "new");
    const auto foreground = directory.path / "new" / "current.gif";
    write_warm_gif(foreground, 2, 3000, 3000);
    liltview::test::checkerboard(directory.path / "2.bmp", 31, 23, 7);
    TestRenderer renderer;
    liltview::Viewer viewer(renderer);
    viewer.open(directory.path / "2.bmp");
    ready_neighbors(viewer, 2, std::chrono::seconds(20));
    REQUIRE(viewer.animation_warmup_stats().ready == 2);
    viewer.open(foreground);
    warm_until(
        viewer,
        [&]
        {
            const auto details = viewer.image_details();
            return details && details->path == foreground && details->animation.has_value();
        },
        false, std::chrono::seconds(20));
    CHECK_FALSE(viewer.failed());
    CHECK(viewer.warmed_first_frames_presented() == 0);
    CHECK(viewer.source_size().width == 3000);
    CHECK(viewer.source_size().height == 3000);
    CHECK(viewer.animation_frames_presented() == 1);
    viewer.shutdown();
    CHECK(renderer.textures.empty());
}

TEST_CASE("slow candidate uploads cannot skip the actual warmed first frame", "[viewer][warm][upload]")
{
    EventLoop loop;
    WarmDirectory directory;
    write_warm_gif(directory.path / "2.png", 2, 4, 3, 1, 4); // 40ms first-frame duration.
    TestRenderer renderer;
    liltview::Viewer viewer(renderer);
    open_static_for_warm(loop, viewer, directory.path);
    ready_neighbors(viewer);
    renderer.upload_delay_ms = 50; // Each upload exceeds the source first-frame duration.
    viewer.navigate(1);
    warm_until(viewer, [&] { return viewer.warmed_first_frames_presented() == 1; });
    CHECK(viewer.animation_frames_presented() == 1);
    CHECK(viewer.displayed_frame_index() == 0);
    WarmGui gui;
    gui.draw(viewer, renderer);
    CHECK(renderer.last_drawn_pixel == std::array<std::uint8_t, 4>{255, 0, 0, 255});
    renderer.upload_delay_ms = 0;
    warm_until(viewer, [&] { return viewer.animation_frames_presented() == 2; });
    CHECK(viewer.displayed_frame_index() == 1);
    gui.draw(viewer, renderer);
    CHECK(renderer.last_drawn_pixel == std::array<std::uint8_t, 4>{0, 0, 255, 255});
    viewer.shutdown();
}

TEST_CASE("second warmed texture upload failure falls back before any warmed presentation", "[viewer][warm][upload]")
{
    EventLoop loop;
    WarmDirectory directory;
    write_warm_gif(directory.path / "2.png", 2, 4, 3, 1, 4);
    TestRenderer renderer;
    liltview::Viewer viewer(renderer);
    open_static_for_warm(loop, viewer, directory.path);
    ready_neighbors(viewer);
    renderer.fail_upload_at = renderer.uploads + 2;
    viewer.navigate(1);
    // Stop at the exact failing pump. Ordinary fallback may already be accepted,
    // but it must not consume/present its first frame before a subsequent pump.
    warm_until(viewer, [&] { return !renderer.fail_upload_at; });
    CHECK_FALSE(renderer.fail_upload_at);
    CHECK(viewer.warmed_first_frames_presented() == 0);
    CHECK(viewer.animation_frames_presented() == 0);
    REQUIRE(viewer.image_details());
    CHECK(viewer.image_details()->path == directory.path / "1.bmp");
    CHECK_FALSE(viewer.image_details()->animation);
    WarmGui gui;
    gui.draw(viewer, renderer);
    CHECK(renderer.last_drawn_pixel == std::array<std::uint8_t, 4>{255, 0, 0, 255});
    warm_until(viewer, [&] { return viewer.animation_frames_presented() >= 1; });
    CHECK_FALSE(renderer.fail_upload_at);
    CHECK(viewer.warmed_first_frames_presented() == 0);
    CHECK(viewer.animation_frames_presented() == 1);
    CHECK(viewer.displayed_frame_index() == 0);
    REQUIRE(viewer.image_details());
    CHECK(viewer.image_details()->path == directory.path / "2.png");
    gui.draw(viewer, renderer);
    CHECK(renderer.last_drawn_pixel == std::array<std::uint8_t, 4>{255, 0, 0, 255});
    warm_until(viewer, [&] { return viewer.animation_frames_presented() == 2; });
    CHECK(viewer.warmed_first_frames_presented() == 0);
    CHECK(viewer.displayed_frame_index() == 1);
    gui.draw(viewer, renderer);
    CHECK(renderer.last_drawn_pixel == std::array<std::uint8_t, 4>{0, 0, 255, 255});
    viewer.shutdown();
    CHECK(renderer.textures.empty());
}

// Reviewed GPU-budget scenario: old 256x256 native mip image plus one 600x600
// GIF texture fits a fixed 3MiB cap, while retaining old plus both new textures
// does not. After ordinary first-frame commit releases old, both GIF textures fit.
TEST_CASE("warmed GPU budget rejection preserves the old scene before ordinary playback succeeds",
          "[viewer][warm][budget][upload]")
{
    EventLoop loop;
    WarmDirectory directory;
    const auto old  = directory.path / "1.bmp";
    const auto next = directory.path / "2.png";
    liltview::test::checkerboard(old, 256, 256, 256);
    write_warm_gif(next, 2, 600, 600, 1, 8);
    TestRenderer renderer;
    renderer.image_budget        = 3 * 1024 * 1024;
    renderer.frame_size          = {600, 600};
    renderer.strict_image_budget = true;
    liltview::Viewer viewer(renderer);
    viewer.open(old);
    loop.settle(viewer);
    ready_neighbors(viewer);
    REQUIRE_FALSE(viewer.failed());
    REQUIRE(renderer.textures.size() == 1);
    const auto old_bytes   = renderer.capabilities().image_bytes;
    const auto old_slot    = renderer.textures.begin()->first;
    const auto frame_bytes = liltview::render::image_storage_bytes({{600, 600}, false});
    REQUIRE(old_bytes + frame_bytes <= renderer.image_budget);
    REQUIRE(old_bytes + 2 * frame_bytes > renderer.image_budget);
    REQUIRE(2 * frame_bytes <= renderer.image_budget);
    CHECK(renderer.budget_rejections == 0);

    viewer.navigate(1);
    // Stop in the actual rejected-create pump, before ordinary fallback events
    // can be consumed. The first local texture must also have been cleaned up.
    warm_until(viewer, [&] { return renderer.budget_rejections != 0; });
    CHECK(renderer.budget_rejections == 1);
    CHECK(renderer.rejected_live_bytes == old_bytes + frame_bytes);
    CHECK(renderer.rejected_request_bytes == frame_bytes);
    CHECK(renderer.textures.size() == 1);
    CHECK(renderer.textures.contains(old_slot));
    CHECK(renderer.capabilities().image_bytes == old_bytes);
    CHECK(viewer.warmed_first_frames_presented() == 0);
    CHECK(viewer.animation_frames_presented() == 0);
    CHECK(viewer.loading());
    REQUIRE(viewer.image_details());
    CHECK(viewer.image_details()->path == old);
    CHECK_FALSE(viewer.image_details()->animation);
    WarmGui gui;
    gui.draw(viewer, renderer);
    CHECK(renderer.last_drawn_pixel == std::array<std::uint8_t, 4>{255, 0, 0, 255});
    CHECK(renderer.textures.contains(old_slot));

    warm_until(viewer, [&] { return viewer.animation_frames_presented() >= 1; });
    CHECK_FALSE(viewer.failed());
    CHECK(viewer.warmed_first_frames_presented() == 0);
    CHECK(viewer.animation_frames_presented() == 1);
    CHECK(viewer.displayed_frame_index() == 0);
    CHECK(renderer.budget_rejections == 1);
    CHECK_FALSE(renderer.textures.contains(old_slot));
    CHECK(renderer.capabilities().image_bytes == frame_bytes);
    REQUIRE(viewer.image_details());
    CHECK(viewer.image_details()->path == next);
    gui.draw(viewer, renderer);
    CHECK(renderer.last_drawn_pixel == std::array<std::uint8_t, 4>{255, 0, 0, 255});

    warm_until(viewer, [&] { return viewer.animation_frames_presented() == 2; });
    CHECK_FALSE(viewer.failed());
    CHECK(viewer.displayed_frame_index() == 1);
    CHECK(viewer.warmed_first_frames_presented() == 0);
    CHECK(renderer.budget_rejections == 1);
    CHECK(renderer.capabilities().image_bytes <= renderer.image_budget);
    gui.draw(viewer, renderer);
    CHECK(renderer.last_drawn_pixel == std::array<std::uint8_t, 4>{0, 0, 255, 255});
    viewer.shutdown();
    CHECK(renderer.textures.empty());
    CHECK(renderer.capabilities().image_bytes == 0);
}

TEST_CASE("a prior static cold restore failure cannot suppress a new GIF's neighbor prefetch", "[viewer][warm][restore]")
{
    EventLoop loop;
    WarmDirectory directory;
    liltview::test::checkerboard(directory.path / "1.png", 2048, 2048, 31);
    write_warm_gif(directory.path / "2.gif", 2, 4, 3, 0);
    liltview::test::checkerboard(directory.path / "3.bmp", 17, 13, 7);
    TestRenderer renderer;
    renderer.image_budget = 8 * 1024 * 1024; // The 16MiB source must use cached LODs instead of native GPU residency.
    liltview::Viewer viewer(renderer);
    viewer.open(directory.path / "1.png");
    loop.settle(viewer);
    REQUIRE_FALSE(viewer.failed());
    WarmGui gui;
    renderer.allow_upload = false;
    gui.draw(viewer, renderer); // Complete FIT atlas is ready; native center may only be prefetched in RAM.
    REQUIRE(viewer.detail_ready());
    viewer.actual_size();
    gui.draw(viewer, renderer);
    REQUIRE(viewer.detail_loading());
    CHECK_FALSE(viewer.interaction_ready());
    CHECK(viewer.image_loaded());
    renderer.fail_upload  = true;
    renderer.allow_upload = true;
    const auto deadline   = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!viewer.failed())
    {
        (viewer.pump(), viewer.prepare_loading_view());
        gui.draw(viewer, renderer);
        check_background_quota(viewer);
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        SDL_Event event;
        SDL_WaitEventTimeout(&event, 2);
    }
    CHECK_FALSE(renderer.fail_upload);
    CHECK_FALSE(viewer.loading()); // Failure was cache restoration after full decode, not initial loading.
    REQUIRE(viewer.image_details());
    CHECK(viewer.image_details()->path == directory.path / "1.png");
    viewer.open(directory.path / "2.gif");
    warm_until(viewer,
               [&]
               {
                   const auto details = viewer.image_details();
                   return details && details->animation && details->path == directory.path / "2.gif";
               });
    warm_until(viewer, [&] { return !viewer.navigation().scanning && !viewer.prefetch_pending(); });
    CHECK_FALSE(viewer.failed());
    CHECK(viewer.image_details()->path == directory.path / "2.gif");
    REQUIRE(viewer.prefetch_stats().completed > 0);
    viewer.navigate(1);
    loop.settle(viewer);
    REQUIRE_FALSE(viewer.failed());
    CHECK(viewer.image_details()->path == directory.path / "3.bmp");
    CHECK(viewer.last_source_bytes_read() == 0); // New GIF's C neighbor was genuinely prefetched.
    viewer.shutdown();
    CHECK(renderer.textures.empty());
}

TEST_CASE("initial native first view stays hidden until current DPI viewport is covered in both upload modes", "[viewer][native][first_view]")
{
    EventLoop loop;
    InformationGui gui;
    DetailsDirectory directory;
    const auto file = directory.path / "first-native.png";
    liltview::test::checkerboard(file, 2048, 2048, 31);
    for (const auto mode : {liltview::Viewer::StaticUploadMode::pipeline, liltview::Viewer::StaticUploadMode::batch})
    {
        CAPTURE(mode == liltview::Viewer::StaticUploadMode::batch);
        TestRenderer renderer;
        renderer.image_budget = 8ULL*1024*1024;
        renderer.strict_image_budget = true;
        liltview::Viewer viewer(renderer);
        viewer.set_static_upload_mode(mode);
        viewer.open(file);
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while (!viewer.static_load_stats().tiles)
        {
            viewer.pump(); // Deliberately do not commit the old/default layout.
            REQUIRE_FALSE(viewer.failed());
            REQUIRE(std::chrono::steady_clock::now()<deadline);
            SDL_Delay(1);
        }
        REQUIRE(viewer.loading());
        REQUIRE_FALSE(viewer.has_visible_image());
        viewer.actual_size();
        do
        {
            viewer.pump();
            viewer.set_viewport({0,0},{320,240},2);
            if (viewer.loading()) CHECK_FALSE(viewer.has_visible_image());
            REQUIRE_FALSE(viewer.failed());
            REQUIRE(std::chrono::steady_clock::now()<deadline);
            SDL_Delay(1);
        } while (!viewer.settled());
        REQUIRE(viewer.has_visible_image());
        CHECK(viewer.cache_stats().gpu_restores>0);
        CHECK(viewer.static_load_stats().ready_ms>=viewer.static_load_stats().last_upload_ms);
        CHECK(viewer.static_load_stats().ready_ms>=viewer.static_load_stats().prepare_done_ms);
        if (mode==liltview::Viewer::StaticUploadMode::batch)
            CHECK(viewer.static_load_stats().first_upload_ms>=viewer.static_load_stats().prepare_done_ms);
        gui.frame(viewer,2);
        CHECK(viewer.presented_view().scale()==Catch::Approx(1));
        CHECK_FALSE(viewer.detail_loading());
        viewer.shutdown();
        CHECK(renderer.textures.empty());
    }
}
