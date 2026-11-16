// First-load comparison. Fresh process per sample, identical full-quality output.
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <iterator>
#include <spdlog/spdlog.h>
#include <thread>
#include <vector>
#ifdef _WIN32
    #include <windows.h>

    #include <psapi.h>
#else
    #include <sys/resource.h>
#endif
#include "platform/sdl_render_host.hpp"
#include "renderer/renderer.hpp"
#include "settings/settings.hpp"
#include "viewer/viewer.hpp"
using namespace liltview;
using Clock = std::chrono::steady_clock;
static double since(Clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
static double percentile(std::vector<double> values, double q)
{
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(std::ceil(q * values.size())) - 1];
}
static std::pair<std::uint64_t, double> process_usage()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
    FILETIME creation{}, exit{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    const auto ticks = [](FILETIME t)
    {
        return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return {memory.PeakWorkingSetSize, (ticks(kernel) + ticks(user)) / 10000.0};
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return {static_cast<std::uint64_t>(usage.ru_maxrss) *
    #ifdef __APPLE__
                1,
    #else
                1024,
    #endif
            (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 +
                (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0};
#endif
}
int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "Usage: loading_benchmark IMAGE SETTINGS_YAML REPORT_JSON [VSYNC=1] [FRAME_MS=16.667] [SCREENSHOT_PPM]\n");
        return 2;
    }
    try
    {
        std::ifstream config_file(std::filesystem::path(argv[2]), std::ios::binary);
        if (!config_file)
            throw std::runtime_error("Cannot read benchmark settings");
        const std::string yaml((std::istreambuf_iterator<char>(config_file)), {});
        const auto settings  = parse_settings(yaml);
        const bool batch     = settings.static_upload_mode == StaticUploadMode::batch;
        const bool vsync     = argc <= 4 || std::string_view(argv[4]) != "0";
        const double cadence = argc > 5 ? std::stod(argv[5]) : 16.667;
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
            throw std::runtime_error(SDL_GetError());
        auto *window = SDL_CreateWindow("Loading benchmark", 1600, 1000, SDL_WINDOW_HIDDEN | prepare_render_window());
        if (!window)
            throw std::runtime_error(SDL_GetError());
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        {
            auto renderer = render::create_renderer({render::SurfaceKind::sdl_window, window, 128ULL * 1024 * 1024, vsync});
            if (!initialize_imgui_platform(window))
                throw std::runtime_error(SDL_GetError());
            {
                Viewer viewer(*renderer);
                viewer.set_static_upload_mode(batch ? Viewer::StaticUploadMode::batch : Viewer::StaticUploadMode::pipeline);
                viewer.set_static_cache_compression(settings.static_cache_compression);
                std::vector<double> work, pump, submit, present, event_delay;
                std::ofstream frames(std::string(argv[3]) + ".frames.csv");
                frames << "frame,pump_ms,layout_ms,submit_ms,present_ms,work_ms,loading,tiles\n";
                const auto event_type = SDL_RegisterEvents(1);
                std::jthread events(
                    [&](std::stop_token stop)
                    {
                        while (!stop.stop_requested())
                        {
                            SDL_Event e{};
                            e.type           = event_type;
                            e.user.timestamp = SDL_GetTicksNS();
                            SDL_PushEvent(&e);
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        }
                    });
                const auto begin = Clock::now();
                viewer.open(std::filesystem::path(argv[1]));
                std::size_t count = 0, partial = 0;
                do
                {
                    const auto tick = Clock::now();
                    SDL_Event e;
                    while (SDL_PollEvent(&e))
                    {
                        if (e.type == event_type)
                            event_delay.push_back((SDL_GetTicksNS() - e.user.timestamp) / 1e6);
                        else
                            ImGui_ImplSDL3_ProcessEvent(&e);
                    }
                    auto start = Clock::now();
                    viewer.pump();
                    const double p = since(start);
                    pump.push_back(p);
                    renderer->begin_frame();
                    ImGui_ImplSDL3_NewFrame();
                    ImGui::NewFrame();
                    auto &io = ImGui::GetIO();
                    ImGui::SetNextWindowPos({0, 0});
                    ImGui::SetNextWindowSize(io.DisplaySize);
                    ImGui::Begin("loading", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
                    start = Clock::now();
                    viewer.set_viewport({0, 0}, {io.DisplaySize.x, io.DisplaySize.y}, io.DisplayFramebufferScale.x);
                    const auto layout = since(start);
                    if (viewer.loading())
                    {
                        ImGui::TextUnformatted("Loading...");
                        if (viewer.has_visible_image())
                            ++partial;
                    }
                    else
                        viewer.draw({0, 0}, {io.DisplaySize.x, io.DisplaySize.y}, io.DisplayFramebufferScale.x, false);
                    ImGui::End();
                    ImGui::Render();
                    start = Clock::now();
                    renderer->render_frame(ImGui::GetDrawData());
                    const double s = since(start);
                    submit.push_back(s);
                    viewer.note_frame_submitted();
                    start = Clock::now();
                    renderer->present();
                    const double v = since(start);
                    present.push_back(v);
                    viewer.note_frame_presented();
                    const auto w = since(tick);
                    work.push_back(w);
                    frames << count++ << ',' << p << ',' << layout << ',' << s << ',' << v << ',' << w << ',' << viewer.loading()
                           << ',' << viewer.static_load_stats().tiles << '\n';
                    if (viewer.failed())
                        throw std::runtime_error("First load failed");
                    if (since(begin) > 120000)
                        throw std::runtime_error("First load timeout");
                    if (!viewer.settled() && cadence > 0)
                        std::this_thread::sleep_until(
                            tick +
                            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(cadence)));
                } while (!viewer.settled());
                events.request_stop();
                events.join();
                SDL_Event remaining;
                while (SDL_PollEvent(&remaining))
                    if (remaining.type == event_type)
                        event_delay.push_back((SDL_GetTicksNS() - remaining.user.timestamp) / 1e6);
                // A stable pixel hash proves both modes produced identical complete output.
                const auto pixels  = renderer->readback();
                if (pixels.size.width <= 0 || pixels.size.height <= 0 ||
                    pixels.rgb.size() != static_cast<std::size_t>(pixels.size.width) * pixels.size.height * 3)
                    throw std::runtime_error("Invalid final framebuffer readback");
                if (argc > 6)
                {
                    std::ofstream screenshot(std::filesystem::path(argv[6]), std::ios::binary);
                    screenshot << "P6\n" << pixels.size.width << ' ' << pixels.size.height << "\n255\n";
                    screenshot.write(reinterpret_cast<const char *>(pixels.rgb.data()),
                                     static_cast<std::streamsize>(pixels.rgb.size()));
                    if (!screenshot)
                        throw std::runtime_error("Cannot write final framebuffer screenshot");
                }
                std::uint64_t hash = 14695981039346656037ULL;
                for (auto byte : pixels.rgb)
                {
                    hash ^= byte;
                    hash *= 1099511628211ULL;
                }
                // Independent full-image rendering probes, outside first-load timing.
                std::vector<double> fit_submit, fit_present, fit_work, actual_work;
                const auto probe_frame = [&](bool fit) {
                    const auto tick = Clock::now();
                    SDL_Event e;
                    while (SDL_PollEvent(&e)) ImGui_ImplSDL3_ProcessEvent(&e);
                    viewer.pump();
                    renderer->begin_frame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();
                    const auto &io = ImGui::GetIO();
                    ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
                    ImGui::Begin("loading", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
                    viewer.draw({0, 0}, {io.DisplaySize.x, io.DisplaySize.y}, io.DisplayFramebufferScale.x, false);
                    ImGui::End(); ImGui::Render();
                    auto phase = Clock::now();
                    renderer->render_frame(ImGui::GetDrawData());
                    const auto draw_ms = since(phase);
                    phase = Clock::now(); renderer->present();
                    const auto present_ms = since(phase);
                    if (fit) { fit_submit.push_back(draw_ms); fit_present.push_back(present_ms); fit_work.push_back(since(tick)); }
                    else actual_work.push_back(since(tick));
                    if (cadence > 0) std::this_thread::sleep_until(tick +
                        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(cadence)));
                };
                const auto load_usage = process_usage();
                for (unsigned i = 0; i < 60; ++i) probe_frame(true);
                const auto zoom_start = Clock::now();
                viewer.actual_size();
                do {
                    probe_frame(false);
                    if (since(zoom_start) > 10000) throw std::runtime_error("Actual-size restore timeout");
                } while (!viewer.detail_ready());
                const auto actual_ready_ms = since(zoom_start);
                for (unsigned i = 0; i < 30; ++i) probe_frame(false);
                const auto &t = viewer.static_load_stats();
                int interval  = -1;
                SDL_GetRenderVSync(SDL_GetRenderer(window), &interval);
                std::ofstream out(argv[3]);
                out << "{\n\"mode\":\"" << (batch ? "batch" : "pipeline") << "\",\n\"driver\":\""
                    << SDL_GetRendererName(SDL_GetRenderer(window)) << "\",\n\"vsync\":" << interval
                    << ",\n\"frame_cadence_ms\":" << cadence;
                const auto field = [&](const char *name, auto value)
                {
                    out << ",\n\"" << name << "\":" << value;
                };
                field("ready_ms", t.ready_ms);
                field("presented_ms", t.presented_ms);
                field("core_ms", t.core_ms);
                field("first_region_ms", t.first_region_ms);
                field("decode_done_ms", t.decode_done_ms);
                field("prepare_done_ms", t.prepare_done_ms);
                field("first_upload_ms", t.first_upload_ms);
                field("last_upload_ms", t.last_upload_ms);
                field("upload_cpu_ms", t.upload_cpu_ms);
                field("max_upload_slice_ms", t.max_upload_slice_ms);
                field("pack_ms", t.preparation.pack_ms);
                field("lod_ms", t.preparation.lod_ms);
                field("compress_ms", t.preparation.compress_ms);
                field("store_ms", t.preparation.store_ms);
                field("cache_compression", settings.static_cache_compression);
                field("cache_disk_bytes", 0);
                field("workers", t.preparation.workers);
                field("worker_peak", t.preparation.worker_peak);
                field("scratch_limit_bytes", t.preparation.scratch_limit_bytes);
                field("input_peak", t.preparation.input_peak);
                field("output_peak", t.preparation.output_peak);
                field("cache_bytes", t.preparation.cache_bytes);
                field("tiles", t.tiles);
                field("uploaded_bytes", t.uploaded_bytes);
                field("source_bytes", viewer.last_source_bytes_read());
                field("work_p50_ms", percentile(work, .50));
                field("work_p95_ms", percentile(work, .95));
                field("work_p99_ms", percentile(work, .99));
                field("work_max_ms", percentile(work, 1));
                field("pump_max_ms", percentile(pump, 1));
                field("submit_p50_ms", percentile(submit, .50));
                field("submit_p95_ms", percentile(submit, .95));
                field("present_p50_ms", percentile(present, .50));
                field("present_p95_ms", percentile(present, .95));
                const auto cache_stats = viewer.cache_stats();
                field("cache_read_ms", cache_stats.ram.read_ms);
                field("cache_decode_ms", cache_stats.ram.decode_ms);
                field("cache_max_decode_ms", cache_stats.ram.max_decode_ms);
                field("submit_max_ms", percentile(submit, 1));
                field("present_max_ms", percentile(present, 1));
                field("event_p95_ms", percentile(event_delay, .95));
                field("event_max_ms", percentile(event_delay, 1));
                const auto usage = process_usage();
                field("peak_rss_bytes", usage.first);
                field("process_cpu_ms", usage.second);
                field("load_peak_rss_bytes", load_usage.first);
                field("steady_fit_submit_p50_ms", percentile(fit_submit, .50));
                field("steady_fit_submit_p95_ms", percentile(fit_submit, .95));
                field("steady_fit_present_p95_ms", percentile(fit_present, .95));
                field("steady_fit_work_p95_ms", percentile(fit_work, .95));
                field("steady_fit_work_max_ms", percentile(fit_work, 1));
                field("actual_ready_ms", actual_ready_ms);
                field("actual_work_p95_ms", percentile(actual_work, .95));
                field("actual_work_max_ms", percentile(actual_work, 1));
                field("frames", count);
                field("partial_frames", partial);
                field("image_bytes", renderer->capabilities().image_bytes);
                out << ",\n\"pixel_hash\":\"" << hash << "\"\n}\n";
                if (!out)
                    throw std::runtime_error("Cannot write benchmark report");
                viewer.shutdown();
            }
            ImGui_ImplSDL3_Shutdown();
        }
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    } catch (const std::exception &e)
    {
        std::fprintf(stderr, "Loading benchmark: %s\n", e.what());
        return 1;
    }
}
