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
// Force a cache miss to span several frames instead of relying on disk speed.
class SlowRenderer final : public render::Renderer
{
public:
    explicit SlowRenderer(render::Renderer &inner) :
        inner_(inner)
    {
    }
    unsigned delay_ms = 0;
    bool fail_upload  = false;
    render::Capabilities capabilities() const override
    {
        return inner_.capabilities();
    }
    render::Size framebuffer_size() const override
    {
        return inner_.framebuffer_size();
    }
    render::Image create_image(render::ImageDesc desc) override
    {
        return inner_.create_image(desc);
    }
    void update_image(render::Image image, render::Rect rect, render::Pixels pixels) override
    {
        if (fail_upload)
        {
            fail_upload = false;
            throw render::Error(render::ErrorCode::backend_failure, "Injected cache restore failure");
        }
        if (delay_ms)
            SDL_Delay(delay_ms);
        inner_.update_image(image, rect, pixels);
    }
    void destroy_image(render::Image image) noexcept override
    {
        inner_.destroy_image(image);
    }
    void add_image(ImDrawList *list, render::Image image, render::Point a, render::Point b, render::Point u,
                   render::Point v) override
    {
        inner_.add_image(list, image, a, b, u, v);
    }
    void add_image_quad(ImDrawList *list, render::Image image, const std::array<render::Point, 4> &p,
                        const std::array<render::Point, 4> &uv) override
    {
        inner_.add_image_quad(list, image, p, uv);
    }
    void begin_frame() override
    {
        inner_.begin_frame();
    }
    void render_frame(ImDrawData *data) override
    {
        inner_.render_frame(data);
    }
    void present() override
    {
        inner_.present();
    }
    render::Readback readback() override
    {
        return inner_.readback();
    }

private:
    render::Renderer &inner_;
};
render::Readback transition(Viewer &viewer, SlowRenderer &renderer, const render::Readback &before)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;)
    {
        viewer.pump();
        auto pixels = frame(viewer, renderer, 1);
        check(!viewer.failed(), "Cache restore failed");
        if (!viewer.detail_loading())
            return pixels;
        check(pixels.rgb == before.rgb, "Incomplete target viewport exposed canvas background during cache restore");
        check(!viewer.settled(), "Pending camera transition was reported settled");
        check(std::chrono::steady_clock::now() < deadline, "Atomic viewport restore timed out");
    }
}
void exercise_restore(render::Renderer &real, const std::filesystem::path &folder)
{
    for (unsigned mode : {0u, 1u, 2u})
    {
        const bool light = mode == 1, transparent = mode == 2;
        if (light)
            ImGui::StyleColorsLight();
        else
            ImGui::StyleColorsDark();
        const auto path = folder / "restore.png";
        fixture(path, transparent);
        SlowRenderer renderer(real);
        Viewer viewer(renderer, Viewer::CameraPolicy::atomic);
        viewer.open(path);
        const auto fit = settle(viewer, renderer, 1);
        std::filesystem::remove(path); // All following operations are cache-only.
        renderer.delay_ms = 5;         // Each 4 ms prepare pass can upload at most one tile.
        viewer.actual_size();
        if (transparent)
            viewer.zoom(-4, {255.5f, 254.5f}); // Cache LOD1, then upgrade it to native below.
        const auto partial = frame(viewer, renderer, 1);
        check(viewer.detail_loading(), "Test did not force a multi-frame cache restore");
        check(partial.rgb == fit.rgb, "Zoom immediately exposed partially restored native tiles");
        // Cancelling a pending target must not publish its partial textures.
        viewer.fit();
        check(frame(viewer, renderer, 1).rgb == fit.rgb, "Cancelled target replaced the presented viewport");
        viewer.actual_size();
        if (transparent)
            viewer.zoom(-4, {255.5f, 254.5f});
        auto native = transition(viewer, renderer, fit);
        if (transparent)
        {
            const auto coarse = native;
            viewer.actual_size();
            check(frame(viewer, renderer, 1).rgb == coarse.rgb, "LOD upgrade modified the presented alpha textures");
            native = transition(viewer, renderer, coarse);
        }
        verify(native, transparent);

        // Change the desired camera while an older target is still being prepared.
        viewer.pan(-768, 0);
        const auto pan = frame(viewer, renderer, 1);
        check(viewer.detail_loading(), "Pan cache miss did not span frames");
        check(pan.rgb == native.rgb, "Pan exposed background instead of retaining the last complete frame");
        viewer.pan(-767, 0);
        const auto latest = transition(viewer, renderer, native);
        verify(latest, transparent);
        const auto at         = (static_cast<std::size_t>(100) * latest.size.width + 100) * 3 + 1;
        const auto background = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_WindowBg]);
        const auto low        = transparent ? static_cast<unsigned>((background >> IM_COL32_G_SHIFT) & 255u) : 0u;
        check(latest.rgb[at] == (native.rgb[at] == 255 ? low : 255u),
              "An obsolete viewport was committed instead of the latest camera");

        viewer.pan(0, -767);
        renderer.fail_upload        = true;
        const auto failure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!viewer.failed())
        {
            const auto failed = frame(viewer, renderer, 1);
            check(failed.rgb == latest.rgb, "Restore failure destroyed the last complete viewport");
            check(std::chrono::steady_clock::now() < failure_deadline, "Injected restore failure was not reported");
            SDL_Delay(1);
        }
        viewer.pan(0, -2); // A new camera request retries without source decoding.
        verify(transition(viewer, renderer, latest), transparent);
        viewer.fit();
        transition(viewer, renderer, latest);
        viewer.shutdown();
        check(real.capabilities().image_bytes == 0, "Atomic restore leaked pinned textures");
    }
}

void preheat(Viewer &viewer, render::Renderer &renderer)
{
    const auto deadline   = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    unsigned stable       = 0;
    std::uint64_t uploads = 0;
    while (stable < 3)
    {
        viewer.pump();
        frame(viewer, renderer, 1);
        check(!viewer.failed(), "Cache preheat failed");
        const auto stats = viewer.cache_stats();
        if (!stats.ram.active && stats.ram.queued == 0 && !stats.upload_pending && stats.gpu_restores == uploads)
            ++stable;
        else
            stable = 0;
        uploads = stats.gpu_restores;
        check(std::chrono::steady_clock::now() < deadline, "Cache preheat did not become idle");
        SDL_Delay(1);
    }
}
void motion_fixture(const std::filesystem::path &path, bool transparent)
{
    FILE *file = std::fopen(path.c_str(), "wb");
    check(file, "Cannot create motion fixture");
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info  = png_create_info_struct(png);
    check(png && info, "PNG allocation failed");
    if (setjmp(png_jmpbuf(png)))
        throw std::runtime_error("Motion fixture encode failed");
    png_init_io(png, file);
    constexpr unsigned width = 4097, height = 3073;
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    std::vector<unsigned char> row(width * 4);
    for (unsigned y = 0; y < height; ++y)
    {
        for (unsigned x = 0; x < width; ++x)
        {
            row[x * 4]     = 255;
            row[x * 4 + 1] = ((x / 64 + y / 64) & 1u) ? 255 : 0;
            row[x * 4 + 2] = 0;
            row[x * 4 + 3] = transparent ? 128 : 255;
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    std::fclose(file);
}
void verify_motion(const Viewer &viewer, const render::Readback &pixels, bool transparent,
                   const std::array<unsigned, 3> &background)
{
    const auto source = viewer.source_size();
    const auto &view  = viewer.presented_view();
    const auto q      = view.quad({0, 0, source.width, source.height}, source);
    const auto blend  = [&](unsigned value, unsigned channel)
    {
        return transparent ? static_cast<int>((value * 128 + background[channel] * 127 + 127) / 255) : static_cast<int>(value);
    };
    unsigned checked = 0;
    for (unsigned y = 4; y < 505; y += 7)
        for (unsigned x = 4; x < 507; x += 7)
        {
            const double sx     = (x + 0.5 - q[0].x) * source.width / (q[1].x - q[0].x);
            const double sy     = (y + 0.5 - q[0].y) * source.height / (q[3].y - q[0].y);
            const double margin = std::max(4.0, 4.0 / view.scale());
            if (sx < margin || sy < margin || sx >= source.width - margin || sy >= source.height - margin)
                continue;
            const auto at = (static_cast<std::size_t>(y) * pixels.size.width + x) * 3;
            check(std::abs(static_cast<int>(pixels.rgb[at]) - blend(255, 0)) <= 2 &&
                      std::abs(static_cast<int>(pixels.rgb[at + 2]) - blend(0, 2)) <= 2,
                  "Covered camera motion exposed a missing tile or blended the image twice");
            if (std::fmod(sx, 64) > margin && std::fmod(sx, 64) < 64 - margin && std::fmod(sy, 64) > margin &&
                std::fmod(sy, 64) < 64 - margin)
            {
                const bool green = ((static_cast<unsigned>(sx) / 64 + static_cast<unsigned>(sy) / 64) & 1u) != 0;
                check(std::abs(static_cast<int>(pixels.rgb[at + 1]) - blend(green ? 255 : 0, 1)) <= 2,
                      "Image pixels did not follow the presented camera transform");
            }
            ++checked;
        }
    check(checked > 100, "Too few motion pixels verified");
}
void exercise_motion(render::Renderer &real, const std::filesystem::path &folder)
{
    for (bool transparent : {false, true})
    {
        if (transparent)
            ImGui::StyleColorsDark();
        else
            ImGui::StyleColorsLight();
        const auto path = folder / "motion.png";
        motion_fixture(path, transparent);
        SlowRenderer renderer(real);
        Viewer viewer(renderer);
        viewer.open(path);
        const auto fit = settle(viewer, renderer, 1);
        const auto at  = (static_cast<std::size_t>(10) * fit.size.width + 10) * 3;
        const std::array<unsigned, 3> background{{fit.rgb[at], fit.rgb[at + 1], fit.rgb[at + 2]}};
        std::filesystem::remove(path);
        viewer.actual_size();
        settle(viewer, renderer, 1);
        preheat(viewer, renderer);
        const auto before = viewer.presented_view();
        renderer.delay_ms = 5;
        viewer.pan(-600, 0); // Beyond the GPU halo: the final camera is not ready yet.
        auto first = frame(viewer, renderer, 1);
        check(viewer.detail_loading(), "Motion test did not force an unfinished restore");
        check(!viewer.presented_view().same_view(before),
              "Cache restore unnecessarily froze the entire camera despite resident high-resolution coverage");
        verify_motion(viewer, first, transparent, background);
        viewer.pan(-300, 0); // The latest target replaces the pending destination.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!viewer.settled())
        {
            verify_motion(viewer, frame(viewer, renderer, 1), transparent, background);
            check(!viewer.failed(), "Covered motion restore failed");
            check(std::chrono::steady_clock::now() < deadline, "Covered motion did not settle");
            SDL_Delay(1);
        }
        preheat(viewer, renderer);
        const auto native = viewer.presented_view();
        viewer.zoom(-6, {255.5f, 254.5f});
        auto zoom = frame(viewer, renderer, 1);
        check(viewer.detail_loading(), "Zoom did not require new GPU coverage");
        check(viewer.presented_view().scale() < native.scale(),
              "Zoom remained completely frozen while usable high-resolution textures were resident");
        verify_motion(viewer, zoom, transparent, background);
        while (!viewer.settled())
        {
            verify_motion(viewer, frame(viewer, renderer, 1), transparent, background);
            check(!viewer.failed(), "Covered zoom restore failed");
            check(std::chrono::steady_clock::now() < deadline, "Covered zoom did not settle");
            SDL_Delay(1);
        }
        viewer.shutdown();
        check(real.capabilities().image_bytes == 0, "Covered camera motion leaked GPU resources");
    }
}

void exercise_interaction(render::Renderer &renderer, const std::filesystem::path &folder)
{
    const auto path = folder / "interaction.png";
    fixture(path, false);
    Viewer viewer(renderer);
    viewer.open(path);
    settle(viewer, renderer, 1);
    std::filesystem::remove(path);
    viewer.actual_size();
    settle(viewer, renderer, 1);
    preheat(viewer, renderer);
    const auto before = viewer.cache_stats();
    std::vector<double> elapsed;
    for (unsigned i = 0; i < 32; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        viewer.pan(-1, 0);
        auto pixels = frame(viewer, renderer, 1);
        elapsed.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        check(!viewer.detail_loading(), "Small pan stalled despite a resident neighbor ring");
        verify(pixels, false);
        check(viewer.cache_stats().indexed_tiles < 128, "Interaction scanned the full source tile table");
    }
    for (unsigned i = 0; i < 16; ++i)
    {
        viewer.zoom(1, {255.5f, 254.5f});
        frame(viewer, renderer, 1);
        check(!viewer.detail_loading(), "Resident zoom unnecessarily restored textures");
        viewer.zoom(-1, {255.5f, 254.5f});
        frame(viewer, renderer, 1);
        check(!viewer.detail_loading(), "Resident reverse zoom unnecessarily restored textures");
    }
    const auto after = viewer.cache_stats();
    check(after.ram.reads == before.ram.reads, "Warm pan/zoom reread or decompressed disk cache");
    check(after.gpu_restores == before.gpu_restores, "Warm pan/zoom reuploaded resident textures");
    check(after.ram.cached_bytes <= TileReader::default_budget, "Decoded tile RAM cache exceeded budget");
    const auto cold_start = std::chrono::steady_clock::now();
    viewer.pan(-768, 0);
    verify(settle(viewer, renderer, 1), false);
    const auto cold_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cold_start).count();
    std::sort(elapsed.begin(), elapsed.end());
    std::printf("interaction warm pan P50=%.3fms P95=%.3fms (offscreen readback included), cold transition=%.3fms, "
                "warm extra disk reads=0, warm extra GPU uploads=0\n",
                elapsed[elapsed.size() / 2], elapsed[elapsed.size() * 95 / 100], cold_ms);
    viewer.shutdown();
    check(renderer.capabilities().image_bytes == 0, "Interaction cache leaked GPU textures");
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
int main(int argc, char **argv)
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
        if (argc > 1 && std::string(argv[1]) == "--restore")
            exercise_restore(*renderer, folder);
        else if (argc > 1 && std::string(argv[1]) == "--interaction")
            exercise_interaction(*renderer, folder);
        else if (argc > 1 && std::string(argv[1]) == "--motion")
            exercise_motion(*renderer, folder);
        else
            exercise(*renderer, folder);
        renderer.reset();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        std::filesystem::remove_all(folder);
        if (argc > 1 && std::string(argv[1]) == "--interaction")
            std::puts("PASS bounded background cache, indexed interaction and zero warm reuploads");
        else if (argc > 1 && std::string(argv[1]) == "--motion")
            std::puts("PASS complete high-resolution camera motion while background restoration is pending");
        else if (argc > 1)
            std::puts("PASS atomic zoom/pan restore, latest-camera commit, cancellation and failure retention");
        else
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
