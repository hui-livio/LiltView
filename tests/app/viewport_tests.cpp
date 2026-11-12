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

TEST_CASE("keyboard zoom uses the canvas center and the same steps as the wheel", "[viewport][shortcut]")
{
    for (const float density : {1.0f, 1.25f, 1.5f, 2.0f})
        for (lv_orientation orientation = 1; orientation <= 8; ++orientation)
        {
            CAPTURE(density, orientation);
            Viewport view;
            view.update({17, 31}, {529, 415}, density, {8192, 4096}, orientation);
            view.actual_size();
            view.pan(64, -64);
            auto wheel = view;
            wheel.zoom(1, {273, 223});
            view.zoom(1);
            CHECK(view.same_view(wheel));
            CHECK(std::abs(view.scale() - 1.25) < 1e-12);
            wheel.zoom(-1, {273, 223});
            view.zoom(-1);
            CHECK(view.same_view(wheel));
            CHECK(std::abs(view.scale() - 1) < 1e-12);
        }
}

TEST_CASE("keyboard zoom respects fit and maximum scale even when repeated", "[viewport][shortcut]")
{
    Viewport view;
    view.zoom(1); // No image loaded yet.
    CHECK(view.scale() == 1);
    view.update({0, 0}, {512, 512}, 1, {8192, 4096}, 1);
    const double fit = view.scale();
    for (unsigned i = 0; i < 100; ++i)
        view.zoom(1);
    CHECK(view.scale() == 32);
    for (unsigned i = 0; i < 100; ++i)
        view.zoom(-1);
    CHECK(view.scale() == fit);
}

TEST_CASE("directional panning moves image content on screen and preserves image bounds", "[viewport][shortcut]")
{
    for (const float density : {1.0f, 1.25f, 1.5f, 2.0f})
        for (lv_orientation orientation = 1; orientation <= 8; ++orientation)
        {
            CAPTURE(density, orientation);
            Viewport original;
            original.update({0, 0}, {512, 512}, density, {8192, 4096}, orientation);
            auto fitted = original;
            fitted.pan(64, 64);
            CHECK(fitted.same_view(original)); // No room to pan a fitted image.
            original.actual_size();
            const auto before = original.quad({0, 0, 8192, 4096}, {8192, 4096});
            for (const auto delta : {render::Point{-64, 0}, {64, 0}, {0, -64}, {0, 64}})
            {
                auto view = original;
                view.pan(delta.x, delta.y);
                const auto after = view.quad({0, 0, 8192, 4096}, {8192, 4096});
                CHECK(std::abs(after[0].x - before[0].x - delta.x) < 0.001f);
                CHECK(std::abs(after[0].y - before[0].y - delta.y) < 0.001f);
                view.pan(delta.x * 1000000, delta.y * 1000000);
                const auto edge = view;
                view.pan(delta.x, delta.y);
                CHECK(view.same_view(edge)); // Holding an arrow cannot pan past the edge.
            }
        }
}

TEST_CASE("quarter turns rotate screen coordinates after every EXIF orientation without changing native detail",
          "[viewport][rotation][orientation][sharpness]")
{
    for (const float density : {1.0f, 1.25f, 1.5f, 2.0f})
        for (lv_orientation orientation = 1; orientation <= 8; ++orientation)
        {
            CAPTURE(density, orientation);
            Viewport original;
            original.update({0, 0}, {512 / density, 512 / density}, density, {60000, 40000}, orientation);
            original.actual_size();
            original.pan(64, -32);
            const auto target  = original.target(256, true);
            const auto before  = original.quad(target.rect, target.canvas);
            const float center = 256 / density;
            for (const int turns : {-1, 1, 2, 3, 4, -4, 9})
            {
                CAPTURE(turns);
                auto rotated = original;
                rotated.rotate(turns);
                // The next frame's metadata update must preserve manual rotation.
                rotated.update({0, 0}, {512 / density, 512 / density}, density, {60000, 40000}, orientation);
                CHECK(rotated.scale() == original.scale());
                CHECK(rotated.target(256, true) == target);
                const auto after = rotated.quad(target.rect, target.canvas);
                const auto count = (turns % 4 + 4) % 4;
                for (unsigned i = 0; i < before.size(); ++i)
                {
                    auto expected = before[i];
                    for (int turn = 0; turn < count; ++turn)
                        expected = {2 * center - expected.y, expected.x};
                    CHECK(std::abs(after[i].x - expected.x) < 0.001f);
                    CHECK(std::abs(after[i].y - expected.y) < 0.001f);
                    CHECK(std::abs(after[i].x * density - std::round(after[i].x * density)) < 0.001f);
                    CHECK(std::abs(after[i].y * density - std::round(after[i].y * density)) < 0.001f);
                }
                if (count)
                {
                    CHECK_FALSE(rotated.same_layout(original));
                    CHECK(original.toward(rotated, 0.5).same_view(original));
                }
                rotated.rotate(-turns);
                CHECK(rotated.same_view(original));
            }
        }
}

TEST_CASE("rotation refits swapped bounds and preserves the current orientation through fit and actual size",
          "[viewport][rotation][fit]")
{
    Viewport view;
    view.update({0, 0}, {600, 400}, 1, {1200, 600}, LV_ORIENTATION_NORMAL);
    const auto original = view;
    CHECK(view.scale() == 0.5);
    view.rotate(1);
    CHECK(view.fitting());
    CHECK(std::abs(view.scale() - 1.0 / 3) < 1e-12);
    auto quad = view.quad({0, 0, 1200, 600}, {1200, 600});
    CHECK(quad[0].x == 400);
    CHECK(quad[0].y == 0);
    CHECK(quad[2].x == 200);
    CHECK(quad[2].y == 400);
    view.actual_size();
    CHECK(view.scale() == 1);
    view.zoom(1);
    CHECK(view.scale() == 1.25);
    view.rotate(-1);
    CHECK(view.scale() == 1.25);
    view.fit();
    view.update({0, 0}, {600, 400}, 1, {1200, 600}, LV_ORIENTATION_NORMAL);
    CHECK(view.same_view(original));
    view.rotate(-1);
    view.fit();
    view.update({0, 0}, {600, 400}, 1, {1200, 600}, LV_ORIENTATION_NORMAL);
    quad = view.quad({0, 0, 1200, 600}, {1200, 600});
    CHECK(quad[0].x == 200);
    CHECK(quad[0].y == 400);
    view.reset();
    view.rotate(1); // An empty viewer has no persistent rotation to apply later.
    view.update({0, 0}, {600, 400}, 1, {1200, 600}, LV_ORIENTATION_NORMAL);
    CHECK(view.same_view(original));
}

TEST_CASE("rotated tiles retain shared edges and bounded visible native regions", "[viewport][rotation][coverage]")
{
    for (lv_orientation orientation = 1; orientation <= 8; ++orientation)
        for (const int turns : {-1, 1, 2})
        {
            CAPTURE(orientation, turns);
            Viewport view;
            view.update({0, 0}, {600, 400}, 2, {60000, 40000}, orientation);
            view.actual_size();
            view.pan(-64, 32);
            view.rotate(turns);
            const auto target = view.target(256, true);
            CHECK(target.rect.width <= 1792);
            CHECK(target.rect.height <= 1792);
            CHECK(target.rect.x + target.rect.width <= target.canvas.width);
            CHECK(target.rect.y + target.rect.height <= target.canvas.height);
            const auto a = view.quad({target.rect.x, target.rect.y, 128, 128}, target.canvas);
            const auto b = view.quad({target.rect.x + 128, target.rect.y, 128, 128}, target.canvas);
            CHECK(a[1].x == b[0].x);
            CHECK(a[1].y == b[0].y);
            CHECK(a[2].x == b[3].x);
            CHECK(a[2].y == b[3].y);
            CHECK(view.visible(target.rect));
            CHECK_FALSE(view.visible({0, 0, 256, 256}));
        }
}
