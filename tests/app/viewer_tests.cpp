#include "../fixtures/checkerboard.hpp"
#include "viewer/viewer.hpp"

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>

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
    };
    std::unordered_map<std::uint32_t, Texture> textures;
    std::uint32_t next         = 1;
    bool fail_upload           = false;
    unsigned uploads           = 0;
    unsigned upload_delay_ms   = 0;
    unsigned creates           = 0;
    std::uint64_t image_budget = 16 * 1024 * 1024;
    std::uint32_t max_edge     = 4096;

    liltview::render::Capabilities capabilities() const override
    {
        std::uint64_t bytes = 0;
        for (const auto &[id, texture] : textures)
        {
            (void)id;
            bytes += texture.bytes;
        }
        return {liltview::render::Backend::opengl3, max_edge, image_budget, bytes};
    }
    Size framebuffer_size() const override
    {
        return {128, 96};
    }
    Image create_image(liltview::render::ImageDesc desc) override
    {
        REQUIRE(desc.size.width <= max_edge);
        REQUIRE(desc.size.height <= max_edge);
        ++creates;
        const auto slot = next++;
        textures.emplace(slot, Texture{desc.size, liltview::render::image_storage_bytes(desc), desc.magnification});
        return {1, slot, 1};
    }
    void update_image(Image image, Rect rect, Pixels pixels) override
    {
        if (fail_upload)
        {
            fail_upload = false;
            throw liltview::render::Error(liltview::render::ErrorCode::backend_failure, "Injected upload failure");
        }
        liltview::render::validate_upload(textures.at(image.slot).size, rect, pixels);
        ++uploads;
        if (upload_delay_ms)
            SDL_Delay(upload_delay_ms);
    }
    void destroy_image(Image image) noexcept override
    {
        textures.erase(image.slot);
    }
    void add_image(ImDrawList *, Image, Point, Point, Point, Point) override
    {
    }
    void add_image_quad(ImDrawList *, Image, const std::array<Point, 4> &, const std::array<Point, 4> &) override
    {
    }
    void begin_frame() override
    {
    }
    void render_frame(ImDrawData *) override
    {
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
            viewer.pump();
            if (viewer.settled())
                return;
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Viewer did not settle");
            SDL_Event event;
            SDL_WaitEventTimeout(&event, 10);
        } while (true);
    }
};

const std::filesystem::path sample = std::filesystem::path(LILTVIEW_JPEG_FIXTURES) / "testorig.jpg";
} // namespace

TEST_CASE("viewer browses a naturally sorted directory and can leave a deleted or failed target", "[viewer][navigation]")
{
    EventLoop loop;
    TestRenderer renderer;
    const auto folder =
        std::filesystem::temp_directory_path() / ("liltview-navigation-" + std::to_string(SDL_GetPerformanceCounter()));
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
        viewer.pump();
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
    EventLoop loop;
    TestRenderer renderer;
    const auto folder =
        std::filesystem::temp_directory_path() / ("liltview-navigation-symlink-" + std::to_string(SDL_GetPerformanceCounter()));
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
        std::filesystem::temp_directory_path() / ("liltview-navigation-input-" + std::to_string(SDL_GetPerformanceCounter()));
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
    viewer.pump();
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
            viewer.pump();
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
        viewer.pump();
        REQUIRE_FALSE(viewer.failed());
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        SDL_Event event;
        SDL_WaitEventTimeout(&event, viewer.wait_timeout_ms(10));
    }
    for (std::uint64_t index : {0u, 1u})
    {
        while (viewer.displayed_frame_index() != index)
        {
            viewer.pump();
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
        viewer.pump();
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
        viewer.pump();
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
        std::filesystem::temp_directory_path() / ("liltview-native-cache-" + std::to_string(SDL_GetPerformanceCounter()));
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
        std::filesystem::temp_directory_path() / ("liltview-native-snapshot-" + std::to_string(SDL_GetPerformanceCounter()));
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
    auto folder = std::filesystem::temp_directory_path() /
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
        std::filesystem::temp_directory_path() / ("liltview-native-preview-" + std::to_string(SDL_GetPerformanceCounter()));
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

TEST_CASE("native row bands are visible before the complete image finishes decoding", "[viewer][native][stream]")
{
    EventLoop loop;
    TestRenderer renderer;
    renderer.upload_delay_ms = 5; // Stop the UI pump after the first uploaded tile.
    const auto folder =
        std::filesystem::temp_directory_path() / ("liltview-native-first-band-" + std::to_string(SDL_GetPerformanceCounter()));
    std::filesystem::create_directories(folder);
    const auto path = folder / "stream.png";
    liltview::test::checkerboard(path, 1024, 4096, 7);
    liltview::Viewer viewer(renderer);
    viewer.open(path);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!viewer.has_visible_image())
    {
        viewer.pump();
        REQUIRE_FALSE(viewer.failed());
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        SDL_Delay(1);
    }
    CHECK(viewer.loading());
    CHECK_FALSE(viewer.settled());
    CHECK(viewer.source_size().width == 1024);
    CHECK(viewer.source_size().height == 4096);
    renderer.upload_delay_ms = 0;
    loop.settle(viewer);
    CHECK(viewer.detail_ready());
    viewer.shutdown();
    std::filesystem::remove_all(folder);
}
