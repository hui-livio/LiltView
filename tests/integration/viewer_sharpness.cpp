#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <png.h>
#include <stdexcept>
#include <string>
#include <vector>
#include "platform/sdl_render_host.hpp"
#include "renderer/renderer.hpp"
#include "viewer/viewer.hpp"

namespace
{
using namespace liltview;
void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
void fixture(const std::filesystem::path &path, bool transparent, unsigned width = 4097, unsigned height = 3073)
{
    FILE *file = std::fopen(path.c_str(), "wb");
    check(file, "Cannot create PNG fixture");
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info  = png_create_info_struct(png);
    check(png && info, "Cannot allocate PNG encoder");
    if (setjmp(png_jmpbuf(png)))
        throw std::runtime_error("Cannot encode PNG fixture");
    png_init_io(png, file);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    std::vector<unsigned char> row(width * 4);
    for (unsigned y = 0; y < height; ++y)
    {
        for (unsigned x = 0; x < width; ++x)
        {
            const bool white = ((x + y) & 1u) != 0;
            row[x * 4]       = 255;
            row[x * 4 + 1] = row[x * 4 + 2] = white ? 255 : 0;
            row[x * 4 + 3]                  = transparent && !white ? 0 : 255;
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    std::fclose(file);
}
render::Readback frame(Viewer &viewer, render::Renderer &renderer, float density)
{
    renderer.begin_frame();
    ImGui_ImplSDL3_NewFrame();
    auto &io                   = ImGui::GetIO();
    const auto size            = renderer.framebuffer_size();
    io.DisplaySize             = {static_cast<float>(size.width) / density, static_cast<float>(size.height) / density};
    io.DisplayFramebufferScale = {density, density};
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("sharpness", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    // Odd-sized canvas and fractional pan used to leave native tiles off the pixel grid.
    viewer.draw({0, 0}, {511 / density, 509 / density}, density);
    ImGui::End();
    ImGui::Render();
    renderer.render_frame(ImGui::GetDrawData());
    auto pixels = renderer.readback();
    renderer.present();
    return pixels;
}
render::Readback settle(Viewer &viewer, render::Renderer &renderer, float density)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;)
    {
        viewer.pump();
        auto pixels = frame(viewer, renderer, density);
        check(!viewer.failed(), "Viewer decode failed");
        if (viewer.settled())
            return pixels;
        check(std::chrono::steady_clock::now() < deadline, "Viewer decode timed out");
        SDL_Delay(1);
    }
}
void verify(const render::Readback &pixels, bool transparent)
{
    int low = 256, high = -1;
    // A one-pixel source checkerboard must stay one-pixel, not an enlarged FIT preview.
    for (unsigned y = 100; y < 200; ++y)
        for (unsigned x = 100; x < 200; ++x)
        {
            const auto at   = (static_cast<std::size_t>(y) * pixels.size.width + x) * 3;
            const int green = pixels.rgb[at + 1];
            if (!transparent)
                check(pixels.rgb[at] == 255 && (green == 0 || green == 255) && pixels.rgb[at + 2] == green,
                      "100% detail is interpolated or still an enlarged preview");
            if (x > 100)
            {
                const int previous = pixels.rgb[at - 2];
                check(std::abs(green - previous) >= (transparent ? 230 : 250), "Native one-pixel details were lost");
            }
            low  = std::min(low, green);
            high = std::max(high, green);
        }
    if (transparent)
    {
        // Single alpha blend over ImGui's canvas background. A preview underneath
        // causes a second blend and produces a different range/ghosted edges.
        const auto background = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_WindowBg]);
        const int green       = static_cast<int>((background >> IM_COL32_G_SHIFT) & 255u);
        check(std::abs(low - green) <= 2 && high == 255, "Native transparent tiles are composited over a blurry preview");
    }
}
void exercise(render::Renderer &renderer, const std::filesystem::path &folder)
{
    for (bool transparent : {false, true})
    {
        const auto path = folder / (transparent ? "transparent.png" : "opaque.png");
        fixture(path, transparent);
        Viewer viewer(renderer);
        viewer.open(path);
        const auto fit = settle(viewer, renderer, 1);
        const auto at  = (static_cast<std::size_t>(200) * fit.size.width + 200) * 3;
        if (transparent)
            check(fit.rgb[at] >= 130 && fit.rgb[at] <= 145 && fit.rgb[at + 1] >= 130 && fit.rgb[at + 1] <= 145,
                  "Nonuniform transparent native FIT tiles are missing from the shared atlas");
        else
            check(fit.rgb[at] == 255 && fit.rgb[at + 1] >= 126 && fit.rgb[at + 1] <= 130,
                  "Nonuniform native FIT tiles are missing from the shared atlas");
        std::filesystem::remove(path); // Any source re-decode on zoom must now fail.
        viewer.actual_size();
        for (float density : {1.0f, 1.25f, 1.5f, 2.0f})
        {
            settle(viewer, renderer, density);
            viewer.pan(0.25, -0.375);
            verify(settle(viewer, renderer, density), transparent);
        }
        // Exercise more native tiles than the GPU budget can retain, then revisit
        // them without an original source. Only the immutable local cache is used.
        viewer.actual_size();
        settle(viewer, renderer, 1);
        viewer.pan(1e9, 1e9);
        for (unsigned row = 0; row < 4; ++row)
        {
            viewer.pan(1e9, 0);
            for (unsigned column = 0; column < 5; ++column)
            {
                verify(settle(viewer, renderer, 1), transparent);
                const auto caps = renderer.capabilities();
                check(caps.image_bytes <= caps.image_budget_bytes, "Native cache exceeded GPU budget");
                viewer.pan(-768, 0);
            }
            viewer.pan(0, -768);
        }
        viewer.pan(1e9, 1e9);
        verify(settle(viewer, renderer, 1), transparent);
        viewer.fit();
        settle(viewer, renderer, 1);
        viewer.actual_size();
        verify(settle(viewer, renderer, 1), transparent);
        const auto replacement = folder / "replacement.png";
        fixture(replacement, transparent, 573, 601);
        viewer.open(replacement);
        settle(viewer, renderer, 1);
        check(viewer.source_size().width == 573, "Replacement failed under GPU cache pressure");
        viewer.actual_size();
        verify(settle(viewer, renderer, 1), transparent);
        viewer.shutdown();
        check(renderer.capabilities().image_bytes == 0, "Native scene leaked standalone or atlas textures");
    }
}
} // namespace
int main()
{
    SDL_Window *window = nullptr;
    bool platform      = false;
    const auto folder  = std::filesystem::temp_directory_path() /
        ("liltview-sharpness-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try
    {
        check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS), SDL_GetError());
        window = SDL_CreateWindow("sharpness", 512, 512, prepare_render_window() | SDL_WINDOW_HIDDEN);
        check(window, SDL_GetError());
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        auto renderer              = render::create_renderer({render::SurfaceKind::sdl_window, window, 32 * 1024 * 1024, false});
        platform                   = initialize_imgui_platform(window);
        check(platform, "Cannot initialize ImGui platform");
        std::filesystem::create_directories(folder);
        exercise(*renderer, folder);
        renderer.reset();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        std::filesystem::remove_all(folder);
        std::puts("PASS native one-pixel details, fractional pan, HiDPI and transparent preview replacement");
        return 0;
    } catch (const std::exception &error)
    {
        std::fprintf(stderr, "Viewer sharpness: %s\n", error.what());
        if (platform)
            ImGui_ImplSDL3_Shutdown();
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
        std::filesystem::remove_all(folder);
        return 1;
    }
}
