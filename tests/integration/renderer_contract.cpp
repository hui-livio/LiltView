#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <stdexcept>
#include <string_view>
#include <vector>
#include "platform/sdl_render_host.hpp"
#include "renderer/renderer.hpp"

namespace
{
using namespace liltview::render;
void check(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
Readback frame(Renderer &renderer, const std::function<void(ImDrawList *)> &draw)
{
    renderer.begin_frame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    draw(ImGui::GetForegroundDrawList());
    ImGui::Render();
    renderer.render_frame(ImGui::GetDrawData());
    return renderer.readback();
}
std::array<int, 3> pixel(const Readback &frame, unsigned x, unsigned y)
{
    check(x < frame.size.width && y < frame.size.height, "Readback dimensions");
    const auto at = (static_cast<std::size_t>(y) * frame.size.width + x) * 3;
    return {frame.rgb[at], frame.rgb[at + 1], frame.rgb[at + 2]};
}
void near(std::array<int, 3> actual, std::array<int, 3> expected, int tolerance = 1)
{
    for (unsigned i = 0; i < 3; ++i)
        if (std::abs(actual[i] - expected[i]) > tolerance)
        {
            std::fprintf(stderr, "Pixel: actual=(%d,%d,%d) expected=(%d,%d,%d) tolerance=%d\n", actual[0], actual[1], actual[2],
                         expected[0], expected[1], expected[2], tolerance);
            throw std::runtime_error("Unexpected rendered pixel");
        }
}
void exercise(Renderer &renderer, SDL_Window *window)
{
    check(renderer.capabilities().backend == Backend::sdl_renderer && SDL_GetRenderer(window), "Wrong renderer backend");
    check(liltview::prepare_render_window() == 0, "Application window setup unexpectedly requires a graphics API");
    const auto base = renderer.capabilities().image_bytes;
    auto image      = renderer.create_image({{4, 4}, true});
    check(renderer.capabilities().image_bytes == base + 84, "Full mip-chain accounting");
    // Odd pitches and subrectangles must work without retaining caller pixels.
    std::array<std::uint8_t, 4 * 17> data{};
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 2; ++x)
        {
            data[y * 17 + x * 4]     = 230;
            data[y * 17 + x * 4 + 1] = 30;
            data[y * 17 + x * 4 + 2] = 40;
            data[y * 17 + x * 4 + 3] = 255;
        }
    renderer.update_image(image, {0, 0, 2, 4}, {data.data(), data.size(), 17, {2, 4}});
    std::fill(data.begin(), data.end(), 0);
    const auto result = frame(renderer, [&](ImDrawList *list) { renderer.add_image(list, image, {0, 0}, {128, 128}); });
    near(pixel(result, 24, 50), {230, 30, 40});
    near(pixel(result, 110, 50), {14, 16, 19}); // Untouched half is transparent, not uninitialized VRAM.
    renderer.present();
    auto foreign = image;
    ++foreign.owner;
    bool rejected = false;
    try
    {
        renderer.add_image(ImGui::GetForegroundDrawList(), foreign, {0, 0}, {10, 10});
    } catch (const Error &e)
    {
        rejected = e.code == ErrorCode::invalid_handle;
    }
    check(rejected, "Foreign handle accepted");
    renderer.destroy_image(image);
    auto replacement = renderer.create_image({{4, 4}, false});
    check(replacement.generation != image.generation, "Generation reuse");
    rejected = false;
    try
    {
        renderer.update_image(image, {0, 0, 4, 4}, {data.data(), data.size(), 17, {4, 4}});
    } catch (const Error &e)
    {
        rejected = e.code == ErrorCode::invalid_handle;
    }
    check(rejected, "Stale handle accepted");
    renderer.destroy_image(image);
    renderer.destroy_image(replacement);
    check(renderer.capabilities().image_bytes == base, "Destroy accounting");
    rejected = false;
    try
    {
        renderer.create_image({{renderer.capabilities().max_texture_edge + 1, 1}, false});
    } catch (const Error &e)
    {
        rejected = e.code == ErrorCode::dimension_limit;
    }
    check(rejected, "Texture dimension limit");
    rejected = false;
    try
    {
        renderer.create_image({{1024, 1024}, true});
    } catch (const Error &e)
    {
        rejected = e.code == ErrorCode::memory_budget;
    }
    check(rejected && renderer.capabilities().image_bytes == base, "Memory admission leaked storage");
    std::puts("PASS SDL textures, partial uploads, zero initialization, handles, dimensions and budget");
}
void verify_populated_creation(Renderer &renderer, SDL_Window *window)
{
    const auto base = renderer.capabilities().image_bytes;
    auto *device    = SDL_GetRenderer(window);
    const SDL_Rect viewport{3, 5, 79, 83}, clip{9, 11, 50, 53};
    check(SDL_SetRenderViewport(device, &viewport), SDL_GetError());
    check(SDL_SetRenderClipRect(device, &clip), SDL_GetError());
    check(SDL_SetRenderScale(device, 1.25f, 1.5f), SDL_GetError());
    const auto *target = SDL_GetRenderTarget(device);
    for (unsigned stride : {16u, 17u, 20u})
    {
        std::vector<std::uint8_t> data(stride * 3 + 16, 0);
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 4; ++x)
            {
                data[y * stride + x * 4]     = 220;
                data[y * stride + x * 4 + 1] = 40;
                data[y * stride + x * 4 + 2] = 30;
                data[y * stride + x * 4 + 3] = 255;
            }
        auto image = renderer.create_uploaded_image({{4, 4}, true}, {data.data(), data.size(), stride, {4, 4}});
        SDL_Rect actual_view{}, actual_clip{};
        float x = 0, y = 0;
        check(SDL_GetRenderViewport(device, &actual_view) && SDL_GetRenderClipRect(device, &actual_clip) &&
                  SDL_GetRenderScale(device, &x, &y),
              SDL_GetError());
        check(SDL_GetRenderTarget(device) == target && SDL_RectsEqual(&viewport, &actual_view) &&
                  SDL_RectsEqual(&clip, &actual_clip) && x == 1.25f && y == 1.5f,
              "Mipmap generation changed caller render state");
        std::fill(data.begin(), data.end(), 0);
        // Reset only for frame rendering; restore after present for the next iteration.
        const auto result = frame(renderer, [&](ImDrawList *list) { renderer.add_image(list, image, {10, 10}, {110, 110}); });
        near(pixel(result, 50, 50), {220, 40, 30});
        renderer.present();
        renderer.destroy_image(image);
        check(renderer.capabilities().image_bytes == base, "Populated image leaked");
        check(SDL_SetRenderViewport(device, &viewport) && SDL_SetRenderClipRect(device, &clip) &&
                  SDL_SetRenderScale(device, 1.25f, 1.5f),
              SDL_GetError());
    }
    check(SDL_SetRenderViewport(device, nullptr) && SDL_SetRenderClipRect(device, nullptr) && SDL_SetRenderScale(device, 1, 1),
          SDL_GetError());
    const std::array<std::uint8_t, 4> short_source{};
    bool rejected = false;
    try
    {
        renderer.create_uploaded_image({{4, 4}, true}, {short_source.data(), short_source.size(), 16, {4, 4}});
    } catch (const Error &e)
    {
        rejected = e.code == ErrorCode::invalid_argument;
    }
    check(rejected && renderer.capabilities().image_bytes == base, "Invalid initial source allocated/leaked a texture");
    const auto stats = renderer.upload_stats();
    check(stats.direct_uploads > 0 && stats.staging_bytes == 0 && stats.staging_limit == 0 && stats.staged_uploads == 0,
          "SDL uploads unexpectedly depend on application-managed staging");
    std::puts("PASS populated creation, source lifetime, odd pitch and SDL target/viewport/clip/scale preservation");
}
void verify_sampling(Renderer &renderer, SDL_Window *window)
{
    // Nearest magnification and linear minification of the same handle in one frame.
    const std::array<std::uint8_t, 16> board{255, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 255};
    auto image        = renderer.create_uploaded_image({{2, 2}, false, ImageDesc::Magnification::nearest},
                                                       {board.data(), board.size(), 8, {2, 2}});
    const auto result = frame(renderer,
                              [&](ImDrawList *list)
                              {
                                  renderer.add_image(list, image, {0, 0}, {64, 64});
                                  renderer.add_image(list, image, {100, 100}, {101, 101});
                                  const std::array<Point, 4> p{Point{64, 0}, Point{128, 0}, Point{128, 64}, Point{64, 64}};
                                  const std::array<Point, 4> uv{Point{0, 1}, Point{0, 0}, Point{1, 0}, Point{1, 1}};
                                  renderer.add_image_quad(list, image, p, uv);
                              });
    for (unsigned y = 1; y < 63; ++y)
        for (unsigned x = 1; x < 63; ++x)
        {
            const bool white = (x >= 32) != (y >= 32);
            near(pixel(result, x, y), {255, white ? 255 : 0, white ? 255 : 0}, 0);
        }
    // SDL's software triangle rasterizer samples geometry with nearest, even
    // when the texture requests linear. Mipmap generation uses RenderTexture
    // (the filtered blit path), so minified mipmapped images remain filtered.
    const bool software = std::string_view(SDL_GetRendererName(SDL_GetRenderer(window))) == "software";
    near(pixel(result, 100, 100), software ? std::array<int, 3>{255, 0, 0} : std::array<int, 3>{255, 128, 128}, 2);
    near(pixel(result, 70, 6), {255, 255, 255});
    renderer.present();
    renderer.destroy_image(image);
    std::vector<std::uint8_t> checker(16 * 16 * 4, 255);
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x)
            for (unsigned channel = 0; channel < 3; ++channel)
                checker[(y * 16 + x) * 4 + channel] = (x + y) % 2 ? 255 : 0;
    image               = renderer.create_uploaded_image({{16, 16}, true}, {checker.data(), checker.size(), 64, {16, 16}});
    const auto minified = frame(renderer, [&](ImDrawList *list) { renderer.add_image(list, image, {10, 10}, {11, 11}); });
    near(pixel(minified, 10, 10), {128, 128, 128}, 2);
    renderer.present();
    // Updating only the left half must regenerate the whole minification chain
    // without replacing the checkerboard in the untouched right half.
    const std::vector<std::uint8_t> white_half(8 * 16 * 4, 255);
    renderer.update_image(image, {0, 0, 8, 16}, {white_half.data(), white_half.size(), 32, {8, 16}});
    const auto partial = frame(renderer, [&](ImDrawList *list) { renderer.add_image(list, image, {10, 10}, {11, 11}); });
    near(pixel(partial, 10, 10), {192, 192, 192}, 2);
    renderer.present();
    std::fill(checker.begin(), checker.end(), 255);
    renderer.update_image(image, {0, 0, 16, 16}, {checker.data(), checker.size(), 64, {16, 16}});
    const auto updated = frame(renderer, [&](ImDrawList *list) { renderer.add_image(list, image, {10, 10}, {11, 11}); });
    near(pixel(updated, 10, 10), {255, 255, 255});
    renderer.present();
    renderer.destroy_image(image);
    std::puts("PASS per-draw driver sampling, rotated UVs and generated minification levels");
}
void verify_density(Renderer &renderer)
{
    const std::array<std::uint8_t, 4> red{230, 30, 40, 255};
    auto image = renderer.create_uploaded_image({{1, 1}, false}, {red.data(), red.size(), 4, {1, 1}});
    for (const float density : {1.25f, 1.5f, 2.0f})
    {
        renderer.begin_frame();
        ImGui_ImplSDL3_NewFrame();
        auto &io                   = ImGui::GetIO();
        const auto output          = renderer.framebuffer_size();
        io.DisplaySize             = {output.width / density, output.height / density};
        io.DisplayFramebufferScale = {density, density};
        ImGui::NewFrame();
        auto *list = ImGui::GetForegroundDrawList();
        list->PushClipRect({5, 7}, {25, 27}, true);
        renderer.add_image(list, image, {0, 0}, {40, 40});
        list->PopClipRect();
        ImGui::Render();
        renderer.render_frame(ImGui::GetDrawData());
        const auto result = renderer.readback();
        near(pixel(result, static_cast<unsigned>(20 * density), static_cast<unsigned>(22 * density)), {230, 30, 40});
        near(pixel(result, static_cast<unsigned>(30 * density), static_cast<unsigned>(30 * density)), {14, 16, 19});
        near(pixel(result, static_cast<unsigned>(2 * density), static_cast<unsigned>(3 * density)), {14, 16, 19});
        renderer.present();
    }
    renderer.destroy_image(image);
    std::puts("PASS HiDPI physical scaling and clip rectangles at 1.25x/1.5x/2x");
}
void verify_window(Renderer &renderer, SDL_Window *window)
{
    const auto id      = SDL_GetWindowID(window);
    const auto *device = SDL_GetRenderer(window);
    const auto base    = renderer.capabilities().image_bytes;
    const std::array<std::uint8_t, 4> red{230, 30, 40, 255};
    auto image = renderer.create_uploaded_image({{1, 1}, false}, {red.data(), red.size(), 4, {1, 1}});
    for (const bool custom : {true, false, true, false})
    {
        check(SDL_SetWindowBordered(window, !custom) && SDL_SetWindowSize(window, 160, 140), SDL_GetError());
        SDL_SyncWindow(window);
        check(SDL_GetWindowID(window) == id && SDL_GetRenderer(window) == device, "Window decoration replaced the renderer");
        const auto result = frame(renderer,
                                  [&](ImDrawList *list)
                                  {
                                      renderer.add_image(list, image, {0, 0}, {160, 140});
                                      list->AddText({8, 8}, IM_COL32_WHITE, "Preserved font texture");
                                  });
        check(result.size.width == 160 && result.size.height == 140, "Obsolete drawable size");
        near(pixel(result, 80, 70), {230, 30, 40});
        renderer.present();
    }
    renderer.destroy_image(image);
    check(renderer.capabilities().image_bytes == base, "Decoration change leaked image storage");
    check(SDL_SetWindowBordered(window, false), SDL_GetError());
    for (const std::array<float, 3> color : {std::array<float, 3>{0.08f, 0.31f, 0.68f}, {0.38f, 0.70f, 1.0f}})
    {
        renderer.set_opaque_background(color);
        const auto result = frame(renderer, [](ImDrawList *list) { list->AddRectFilled({20, 20}, {40, 40}, IM_COL32_WHITE); });
        near(pixel(result, 0, 0),
             {static_cast<int>(std::lround(color[0] * 255)), static_cast<int>(std::lround(color[1] * 255)),
              static_cast<int>(std::lround(color[2] * 255))});
        check(!result.alpha.empty() && result.alpha[0] == 0, "Rounded-corner diagnostic alpha lost");
        renderer.present();
    }
    std::puts("PASS window/renderer/font/handle preservation, resize, background and transparent corner readback");
}
} // namespace
int main()
{
    SDL_Window *window = nullptr;
    bool platform      = false;
    try
    {
        check(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        window = SDL_CreateWindow("SDL renderer contract", 128, 128, liltview::prepare_render_window() | SDL_WINDOW_HIDDEN);
        check(window, SDL_GetError());
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        auto renderer              = create_renderer({SurfaceKind::sdl_window, window, 1024 * 1024, false});
        platform                   = liltview::initialize_imgui_platform(window);
        check(platform, "SDL platform backend");
        std::printf("SDL_Render driver: %s\n", SDL_GetRendererName(SDL_GetRenderer(window)));
        exercise(*renderer, window);
        verify_populated_creation(*renderer, window);
        verify_sampling(*renderer, window);
        verify_density(*renderer);
        verify_window(*renderer, window);
        renderer.reset();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    } catch (const std::exception &error)
    {
        std::fprintf(stderr, "SDL renderer contract failed: %s\n", error.what());
        if (platform)
            ImGui_ImplSDL3_Shutdown();
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
}
