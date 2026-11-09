#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include "viewer/viewport.hpp"
using namespace liltview;

TEST_CASE("viewport keeps 16K 32K and larger images logical with bounded visible regions", "[viewport]")
{
    for (unsigned edge : {16384u, 32768u, 131072u, 1000000001u})
    {
        CAPTURE(edge);
        Viewport view;
        view.update({0, 0}, {512, 512}, 1, {edge, edge}, 1);
        view.actual_size();
        const auto target = view.target(256);
        CHECK(target.canvas.width == edge);
        CHECK(target.canvas.height == edge);
        CHECK(target.rect.x > 0);
        CHECK(target.rect.width <= 1024);
        CHECK(target.rect.height <= 1024);
        CHECK(static_cast<std::uint64_t>(target.rect.x) + target.rect.width <= edge);
        const auto quad = view.quad(target.rect, target.canvas);
        CHECK(std::isfinite(quad[0].x));
        CHECK(quad[1].x - quad[0].x == static_cast<float>(target.rect.width));
    }
}
TEST_CASE("noninteger zoom keeps subpixel camera precision for very large images", "[viewport][precision]")
{
    Viewport view;
    view.update({0, 0}, {512, 512}, 1, {1000000001u, 1000000001u}, 1);
    view.actual_size();
    view.zoom(-1, {256, 256});
    const auto target = view.target(256);
    const auto before = view.quad(target.rect, target.canvas);
    view.pan(0.25, 0.5);
    const auto after = view.quad(target.rect, target.canvas);
    CHECK(std::abs(after[0].x - before[0].x - 0.25f) < 0.00001f);
    CHECK(std::abs(after[0].y - before[0].y - 0.5f) < 0.00001f);
}
TEST_CASE("all EXIF orientations preserve region bounds and shared tile edges", "[viewport][orientation]")
{
    for (lv_orientation orientation = 1; orientation <= 8; ++orientation)
    {
        Viewport view;
        view.update({0, 0}, {600, 400}, 2, {60000, 40000}, orientation);
        view.actual_size();
        const auto target = view.target(256);
        CHECK(target.canvas.width == 60000);
        CHECK(target.rect.width <= 1792);
        CHECK(target.rect.height <= 1792);
        auto a = view.quad({target.rect.x, target.rect.y, 128, 128}, target.canvas);
        auto b = view.quad({target.rect.x + 128, target.rect.y, 128, 128}, target.canvas);
        CHECK(a[1].x == b[0].x);
        CHECK(a[1].y == b[0].y);
        CHECK(a[2].x == b[3].x);
        CHECK(a[2].y == b[3].y);
    }
}

TEST_CASE("native image pixels align to the physical framebuffer after centering and panning", "[viewport][sharpness]")
{
    for (float density : {1.0f, 1.25f, 1.5f, 2.0f})
        for (lv_orientation orientation = 1; orientation <= 8; ++orientation)
        {
            CAPTURE(density, orientation);
            Viewport view;
            view.update({13.25f, 37.75f}, {524.25f, 546.75f}, density, {32769, 32771}, orientation);
            view.actual_size();
            view.pan(0.25, -0.375);
            const auto target = view.target(256);
            const auto quad   = view.quad(target.rect, target.canvas);
            for (const auto &point : quad)
            {
                CHECK(std::abs(point.x * density - std::round(point.x * density)) < 0.001f);
                CHECK(std::abs(point.y * density - std::round(point.y * density)) < 0.001f);
            }
        }
}

TEST_CASE("GPU readiness excludes offscreen decode-halo tiles at native size", "[viewport][coverage]")
{
    for (float density : {1.0f, 1.25f, 1.5f, 2.0f})
        for (lv_orientation orientation = 1; orientation <= 8; ++orientation)
        {
            CAPTURE(density, orientation);
            Viewport view;
            view.update({0, 0}, {512 / density, 512 / density}, density, {4096, 4096}, orientation);
            view.actual_size();
            unsigned visible = 0;
            for (unsigned row = 6; row < 10; ++row)
                for (unsigned column = 6; column < 10; ++column)
                    visible += view.visible({column * 256, row * 256, 256, 256});
            CHECK(visible == 4); // Decode padding used to turn this into 16 required textures.
        }
}
TEST_CASE("intermediate camera transforms preserve the wheel anchor and reject incompatible layouts", "[viewport][motion]")
{
    Viewport before;
    before.update({0, 0}, {512, 512}, 1, {4096, 4096}, 1);
    before.actual_size();
    auto target = before;
    const render::Point anchor{190, 210};
    target.zoom(-2, anchor);
    const auto full       = lv_rect_u32{0, 0, 4096, 4096};
    const auto origin     = before.quad(full, {4096, 4096});
    const double source_x = (anchor.x - origin[0].x) * 4096 / (origin[1].x - origin[0].x);
    const double source_y = (anchor.y - origin[0].y) * 4096 / (origin[3].y - origin[0].y);
    for (double amount : {0.25, 0.5, 0.75})
    {
        const auto middle = before.toward(target, amount);
        const auto q      = middle.quad(full, {4096, 4096});
        CHECK(middle.scale() < before.scale());
        CHECK(middle.scale() > target.scale());
        CHECK(std::abs((anchor.x - q[0].x) * 4096 / (q[1].x - q[0].x) - source_x) < 0.001);
        CHECK(std::abs((anchor.y - q[0].y) * 4096 / (q[3].y - q[0].y) - source_y) < 0.001);
    }
    CHECK(before.toward(target, 0).same_view(before));
    CHECK(before.toward(target, 1).same_view(target));
    target.update({0, 0}, {600, 400}, 1, {4096, 4096}, 1);
    CHECK_FALSE(before.same_layout(target));
    CHECK(before.toward(target, 0.5).same_view(before));
}
