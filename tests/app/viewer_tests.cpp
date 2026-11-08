#include "../fixtures/checkerboard.hpp"
#include "viewer/viewer.hpp"

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("static details use local textures and rapid pan cancels obsolete regions", "[viewer][region]")
{
    EventLoop loop;
    TestRenderer renderer;
    renderer.max_edge = 128;
    liltview::Viewer viewer(renderer);
    auto folder =
        std::filesystem::temp_directory_path() / ("liltview-viewer-region-" + std::to_string(SDL_GetPerformanceCounter()));
    std::filesystem::create_directories(folder);
    const auto path = folder / "board.png";
    liltview::test::checkerboard(path, 2048, 2048, 128);
    viewer.open(path);
    loop.settle(viewer);
    REQUIRE(viewer.has_image());
    CHECK(viewer.can_actual_size());
    viewer.set_viewport({0, 0}, {512, 512});
    viewer.actual_size();
    loop.settle(viewer);
    REQUIRE_FALSE(viewer.failed());
    CHECK(viewer.detail_ready());
    CHECK(renderer.textures.size() > 2);
    unsigned nearest = 0;
    for (const auto &[id, texture] : renderer.textures)
    {
        (void)id;
        nearest += texture.magnification == liltview::render::ImageDesc::Magnification::nearest;
    }
    CHECK(nearest > 1);
    for (unsigned i = 0; i < 8; ++i)
    {
        viewer.pan(-80, 0);
        viewer.pump();
    }
    loop.settle(viewer);
    CHECK_FALSE(viewer.failed());
    CHECK(viewer.detail_ready());
    viewer.fit();
    viewer.pump();
    CHECK(renderer.textures.size() == 1);
    viewer.shutdown();
    CHECK(renderer.textures.empty());
    std::filesystem::remove_all(folder);
}
TEST_CASE("changed file details cannot mix with an older FIT preview", "[viewer][region][source]")
{
    EventLoop loop;
    TestRenderer renderer;
    liltview::Viewer viewer(renderer);
    auto folder =
        std::filesystem::temp_directory_path() / ("liltview-viewer-source-" + std::to_string(SDL_GetPerformanceCounter()));
    std::filesystem::create_directories(folder);
    const auto path = folder / "board.png";
    liltview::test::checkerboard(path, 1024, 1024, 128);
    viewer.open(path);
    loop.settle(viewer);
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out.put('x');
    }
    viewer.set_viewport({0, 0}, {128, 96});
    viewer.actual_size();
    loop.settle(viewer);
    CHECK(viewer.error() == LV_ERROR_SOURCE_CHANGED);
    CHECK(viewer.has_image());
    CHECK_FALSE(viewer.detail_ready());
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
