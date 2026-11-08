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
TEST_CASE("large image tile coordinates retain subpixel camera precision", "[viewport][precision]")
{
    Viewport view;
    view.update({0, 0}, {512, 512}, 1, {1000000001u, 1000000001u}, 1);
    view.actual_size();
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
