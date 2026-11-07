#include "application.hpp"
#include "logging/configure.hpp"

#include "platform/fonts.hpp"
#include "platform/global_menu.hpp"
#include "platform/sdl_render_host.hpp"
#include "renderer/renderer.hpp"
#include "resources/icon_rgba.hpp"
#include "settings/settings.hpp"
#include "settings/store.hpp"
#include "ui/shell.hpp"
#include "version.hpp"
#include "viewer/viewer.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace liltview
{
namespace
{
void require(bool success, const char *operation)
{
    if (!success)
        throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}

std::filesystem::path utf8_path(std::string_view text)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
}

std::string system_locale()
{
    std::string locale = "en-US";
    auto **preferred   = SDL_GetPreferredLocales(nullptr);
    if (preferred != nullptr)
    {
        if (preferred[0] != nullptr && preferred[0]->language != nullptr)
            locale = preferred[0]->language;
        SDL_free(preferred);
    }
    return locale;
}

class Application
{
    struct DialogResult
    {
        std::mutex mutex;
        std::atomic<bool> ready{false};
        std::string path;
        bool failed = false;
    };

public:
    ~Application()
    {
        // 逆初始化顺序释放；标志位也覆盖启动途中抛异常的情况。
        viewer_.reset();
        if (renderer_)
        {
            renderer_->destroy_image(icon_);
            renderer_.reset();
        }
        if (platform_ready_)
            ImGui_ImplSDL3_Shutdown();
        if (imgui_ready_)
            ImGui::DestroyContext();
        if (window_ != nullptr)
            SDL_DestroyWindow(window_);
        if (sdl_ready_)
            SDL_Quit();
    }

    // 这里只编排启动、主循环和保存；局部菜单对象先于窗口和 SDL 资源析构。
    int run(const LaunchOptions &options)
    {
        initialize(options);
        Shell shell;
        menu_ = current_menu();
        GlobalMenu global_menu(window_, menu_);
        run_event_loop(options, shell, global_menu);
        stop_image_work();
        viewer_.reset();
        save_settings(options.smoke_test);
        spdlog::info("Window closed cleanly after {} frames", rendered_frames_);
        return 0;
    }

private:
    // SDL 元数据必须在视频子系统初始化之前设置，供 Wayland app_id / X11 WM_CLASS 使用。
    void initialize(const LaunchOptions &options)
    {
        require(SDL_SetAppMetadata("LiltView", LILTVIEW_VERSION, LILTVIEW_APP_ID), "SDL_SetAppMetadata");
        require(SDL_Init(SDL_INIT_VIDEO), "SDL_Init");
        sdl_ready_ = true;
        refresh_system_theme();
        prepare_settings(options);
        configure_logging(settings_.log_level);
        spdlog::debug("Log level: {}", log_level_name(settings_.log_level));
        locale_ = system_locale();
        if (options.language)
            settings_.language = *options.language;

        create_window();
        initialize_imgui();
        create_icon();
        load_font();
        viewer_ = std::make_unique<Viewer>(*renderer_);
        if (!options.image_path.empty())
            viewer_->open(utf8_path(options.image_path));
        smoke_started_ = SDL_GetTicks();
        if (settings_.maximized)
            require(SDL_MaximizeWindow(window_), "SDL_MaximizeWindow");
        if (!options.smoke_test)
            require(SDL_ShowWindow(window_), "SDL_ShowWindow");
        spdlog::info("LiltView {} started, app_id={}, video={}, renderer=OpenGL3, texture_limit={}", LILTVIEW_VERSION,
                     LILTVIEW_APP_ID, SDL_GetCurrentVideoDriver(), renderer_->capabilities().max_texture_edge);
    }

    // 窗口和 GL 上下文准备完成后才创建纹理；任一阶段失败都由析构函数回收已完成部分。
    void create_window()
    {
        const auto render_flags = prepare_render_window();
        window_ = SDL_CreateWindow("LiltView", settings_.width, settings_.height,
                                   render_flags | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
        require(window_ != nullptr, "SDL_CreateWindow");
        require(SDL_SetWindowMinimumSize(window_, 640, 480), "SDL_SetWindowMinimumSize");
    }

    void initialize_imgui()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        imgui_ready_    = true;
        auto &io        = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        // 应用设置统一存到用户目录，不在启动目录生成 imgui.ini / 日志。
        io.IniFilename  = nullptr;
        io.LogFilename  = nullptr;
        renderer_       = render::create_renderer({render::SurfaceKind::sdl_window, window_});
        platform_ready_ = initialize_imgui_platform(window_);
        require(platform_ready_, "ImGui SDL3 backend");
    }

    void load_font()
    {
        auto font   = find_application_font();
        auto &io    = ImGui::GetIO();
        bool loaded = load_font_candidate(font);
        if (!loaded && font.bundled)
        {
            spdlog::warn("Bundled font is unreadable or invalid; trying a system font");
            font   = find_system_font();
            loaded = load_font_candidate(font);
        }
        if (loaded)
        {
            chinese_font_ = font.chinese;
            spdlog::info("UI font: {} (Chinese={})", font.path, chinese_font_);
        }
        if (io.Fonts->Fonts.empty())
            io.Fonts->AddFontDefault();
        if (!chinese_font_)
            spdlog::warn("Chinese font unavailable; displaying English UI");
    }

    // 所有 SDL / ImGui 操作限定在主线程；D-Bus 工作线程只提交菜单动作。
    void run_event_loop(const LaunchOptions &options, Shell &shell, GlobalMenu &global_menu)
    {
        last_activity_   = SDL_GetTicks();
        menu_preference_ = settings_.language;
        menu_theme_      = settings_.theme;
        menu_fullscreen_ = is_fullscreen();
        while (!quit_)
        {
            dispatch_global_menu_actions(global_menu, shell);

            if (quit_)
                break;

            if (options.smoke_test)
                inject_smoke_events(shell);

            process_events(options.smoke_test);
            consume_dialog();
            viewer_->pump();

            if (quit_)
                break;

            if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0)
                continue;

            update_visual_style();
            synchronize_menu(global_menu);
            render_frame(options, shell, global_menu.active());
        }
    }

    void dispatch_global_menu_actions(GlobalMenu &global_menu, Shell &shell)
    {
        for (const auto action : global_menu.take_actions())
        {
            apply_menu_setting(action, settings_);
            if (action == MenuAction::quit)
                quit_ = true;
            if (action == MenuAction::fullscreen)
                toggle_fullscreen();
            if (action == MenuAction::about)
                shell.show_about();
            if (action == MenuAction::open)
                open_dialog();
        }
    }

    void stop_image_work()
    {
        viewer_->begin_shutdown();
        while (!viewer_->join_shutdown())
        {
            // Join is polled while platform notifications continue to be processed.
            SDL_Event event;
            if (SDL_WaitEventTimeout(&event, 10))
                ImGui_ImplSDL3_ProcessEvent(&event);
        }
    }

    // 最近有交互时保持响应；静止时延长等待，避免持续空转重绘。
    void process_events(bool smoke_test)
    {
        const bool active = viewer_->loading() || SDL_GetTicks() - last_activity_ < 180 || ImGui::IsAnyItemActive();
        const int wait_ms = smoke_test ? 0 : (active ? 16 : 200);
        SDL_Event event;
        if (!SDL_WaitEventTimeout(&event, wait_ms))
            return;
        do
        {
            handle_event(event);
        } while (SDL_PollEvent(&event));
    }

    void handle_event(const SDL_Event &event)
    {
        ImGui_ImplSDL3_ProcessEvent(&event);
        last_activity_ = SDL_GetTicks();
        if (event.type == SDL_EVENT_SYSTEM_THEME_CHANGED)
            refresh_system_theme();
        if (event.type == SDL_EVENT_QUIT ||
            (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window_)))
            quit_ = true;
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
            handle_shortcut(event.key);
        if (event.type == SDL_EVENT_DROP_FILE && event.drop.data)
            viewer_->open(utf8_path(event.drop.data));
        update_window_settings(event);
    }

    void handle_shortcut(const SDL_KeyboardEvent &key)
    {
        // 文本输入和弹窗优先处理按键，避免应用快捷键抢走编辑操作。
        if (ImGui::GetIO().WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
            return;
        if (key.key == SDLK_F11)
            toggle_fullscreen();
        if (key.key == SDLK_ESCAPE && is_fullscreen())
            toggle_fullscreen();
        if (key.key == SDLK_Q && (key.mod & SDL_KMOD_CTRL) != 0)
            quit_ = true;
        if (key.key == SDLK_O && (key.mod & SDL_KMOD_CTRL) != 0)
            open_dialog();
    }

    static void SDLCALL dialog_callback(void *data, const char *const *files, int)
    {
        // Callback owns a shared reference; no Application/window access after close.
        std::unique_ptr<std::shared_ptr<DialogResult>> reference(static_cast<std::shared_ptr<DialogResult> *>(data));
        auto result = *reference;
        try
        {
            std::lock_guard lock(result->mutex);
            if (files && files[0])
                result->path = files[0];
            result->failed = !files;
        } catch (...)
        {
            result->failed = true;
        }
        result->ready.store(true);
    }

    void open_dialog()
    {
        if (dialog_)
            return;
        dialog_ = std::make_shared<DialogResult>();
        static constexpr SDL_DialogFileFilter filters[]{{"JPEG images", "jpg;jpeg;JPG;JPEG"}, {"All files", "*"}};
        auto *reference = new std::shared_ptr<DialogResult>(dialog_);
        SDL_ShowOpenFileDialog(dialog_callback, reference, window_, filters, 2, nullptr, false);
    }

    void consume_dialog()
    {
        if (!dialog_ || !dialog_->ready.load())
            return;
        std::string path;
        {
            std::lock_guard lock(dialog_->mutex);
            path = dialog_->path;
        }
        if (dialog_->failed)
            spdlog::warn("File dialog failed");
        dialog_.reset();
        if (!path.empty())
            viewer_->open(utf8_path(path));
    }

    void update_window_settings(const SDL_Event &event)
    {
        const auto flags = SDL_GetWindowFlags(window_);
        if (event.type == SDL_EVENT_WINDOW_RESIZED && !is_fullscreen() && (flags & SDL_WINDOW_MAXIMIZED) == 0)
        {
            settings_.width  = event.window.data1;
            settings_.height = event.window.data2;
        }
        if (event.type == SDL_EVENT_WINDOW_MAXIMIZED)
            settings_.maximized = true;
        if (event.type == SDL_EVENT_WINDOW_RESTORED && !is_fullscreen())
            settings_.maximized = false;
    }

    void update_visual_style()
    {
        const float density = std::max(SDL_GetWindowPixelDensity(window_), 1.0f);
        const float scale   = std::clamp(SDL_GetWindowDisplayScale(window_) / density, 1.0f, 4.0f);
        const bool dark     = resolve_theme(settings_.theme, system_theme_) == Theme::dark;
        if (std::abs(scale - applied_scale_) > 0.01f || dark != applied_dark_)
        {
            apply_style(dark, scale);
            applied_scale_ = scale;
            applied_dark_  = dark;
        }
    }

    // SDL 在桌面主题变化时发事件；仅更新观测值，不覆盖用户持久化的三态选择。
    void refresh_system_theme()
    {
        switch (SDL_GetSystemTheme())
        {
        case SDL_SYSTEM_THEME_DARK:
            system_theme_ = Theme::dark;
            break;
        case SDL_SYSTEM_THEME_LIGHT:
            system_theme_ = Theme::light;
            break;
        default:
            system_theme_ = Theme::system;
            break;
        }
    }

    MenuItem current_menu() const
    {
        return build_menu(settings_, resolve_language(settings_.language, locale_, chinese_font_), chinese_font_,
                          is_fullscreen());
    }

    // 菜单数据仅在语言、主题或全屏状态变化时重建，同时更新系统顶栏和窗口回退菜单。
    void synchronize_menu(GlobalMenu &global_menu)
    {
        if (menu_preference_ == settings_.language && menu_theme_ == settings_.theme && menu_fullscreen_ == is_fullscreen())
            return;
        menu_ = current_menu();
        global_menu.update(menu_);
        menu_preference_ = settings_.language;
        menu_theme_      = settings_.theme;
        menu_fullscreen_ = is_fullscreen();
    }

    void render_frame(const LaunchOptions &options, Shell &shell, bool system_menu)
    {
        renderer_->begin_frame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const auto language = resolve_language(settings_.language, locale_, chinese_font_);
        const auto actions  = shell.draw(settings_, language, menu_, system_menu, is_fullscreen(), *renderer_, icon_, *viewer_);
        if (actions.quit)
            quit_ = true;
        if (actions.toggle_fullscreen)
            toggle_fullscreen();
        if (actions.open)
            open_dialog();
        ImGui::Render();
        present_frame(options);
        ++rendered_frames_;
    }

    void present_frame(const LaunchOptions &options)
    {
        renderer_->render_frame(ImGui::GetDrawData());
        capture_smoke_frame(options);
        renderer_->present();
    }

    // 测试动作通过真实 SDL 事件路径执行，避免另写一套退出和弹窗处理逻辑。
    void inject_smoke_events(Shell &shell)
    {
        if (rendered_frames_ == 2)
            require(SDL_SetWindowSize(window_, 960, 640), "Smoke resize");
        if (rendered_frames_ == 4)
            shell.show_about();
        SDL_Event event{};
        if (rendered_frames_ == 6 || rendered_frames_ == 7)
        {
            event.type         = rendered_frames_ == 6 ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID = SDL_GetWindowID(window_);
            event.key.key      = SDLK_ESCAPE;
            event.key.scancode = SDL_SCANCODE_ESCAPE;
        }
        else if (rendered_frames_ >= 11 && (viewer_->settled() || SDL_GetTicks() - smoke_started_ > 10000))
        {
            event.type            = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
            event.window.windowID = SDL_GetWindowID(window_);
        }
        if (event.type != 0)
            SDL_PushEvent(&event);
    }

    void capture_smoke_frame(const LaunchOptions &options)
    {
        if (!options.smoke_test || options.screenshot.empty())
            return;
        if (rendered_frames_ == 5)
        {
            const auto filename = utf8_path(options.screenshot);
            const auto about    = filename.parent_path() / (filename.stem().string() + "-about.ppm");
            save_screenshot(about.string());
        }
        if (rendered_frames_ >= 10 && viewer_->settled() && !smoke_captured_)
        {
            save_screenshot(options.screenshot);
            smoke_captured_ = true;
        }
    }

    bool is_fullscreen() const
    {
        return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
    }

    void toggle_fullscreen()
    {
        if (!SDL_SetWindowFullscreen(window_, !is_fullscreen()))
            spdlog::warn("Cannot toggle fullscreen: {}", SDL_GetError());
    }

    void prepare_settings(const LaunchOptions &options)
    {
        if (!options.config_directory.empty())
            settings_path_ = utf8_path(options.config_directory) / "settings.yaml";
        else if (!options.smoke_test)
        {
            char *directory = SDL_GetPrefPath("LiltView", "LiltView");
            if (directory != nullptr)
            {
                settings_path_ = utf8_path(directory) / "settings.yaml";
                SDL_free(directory);
            }
        }
        if (settings_path_.empty())
            return;
        std::error_code error;
        std::filesystem::create_directories(settings_path_.parent_path(), error);
        if (error)
        {
            spdlog::warn("Cannot create settings directory: {}", error.message());
            settings_path_.clear();
            return;
        }
        std::ifstream input(settings_path_, std::ios::binary);
        std::string data(4097, '\0');
        if (input)
        {
            input.read(data.data(), static_cast<std::streamsize>(data.size()));
            data.resize(static_cast<std::size_t>(input.gcount()));
            settings_ = parse_settings(data);
        }
    }

    void save_settings(bool smoke_test)
    {
        if (settings_path_.empty() || smoke_test)
            return;
        std::error_code error;
        if (!save_settings_file(settings_path_, settings_, error))
            spdlog::warn("Cannot save application settings: {}", error.message());
    }

    void create_icon()
    {
        auto *surface = SDL_CreateSurfaceFrom(resources::icon_size, resources::icon_size, SDL_PIXELFORMAT_RGBA32,
                                              const_cast<unsigned char *>(resources::icon_rgba.data()), resources::icon_size * 4);
        if (surface != nullptr)
        {
            if (!SDL_SetWindowIcon(window_, surface))
                spdlog::debug("Window icon: {}", SDL_GetError());
            SDL_DestroySurface(surface);
        }
        const render::Size size{resources::icon_size, resources::icon_size};
        icon_ = renderer_->create_image({size, true});
        renderer_->update_image(icon_, {0, 0, size.width, size.height},
                                {resources::icon_rgba.data(), resources::icon_rgba.size(), size.width * 4ULL, size});
    }

    void save_screenshot(const std::string &filename)
    {
        const auto pixels = renderer_->readback();
        std::ofstream output(utf8_path(filename), std::ios::binary);
        output << "P6\n" << pixels.size.width << ' ' << pixels.size.height << "\n255\n";
        output.write(reinterpret_cast<const char *>(pixels.rgb.data()), static_cast<std::streamsize>(pixels.rgb.size()));
        if (!output)
            throw std::runtime_error("Cannot save screenshot: " + filename);
    }

    // 应用状态只在主线程读写；GlobalMenu 在 run() 内先销毁，再回收这些图形资源。
    Settings settings_;
    MenuItem menu_;
    std::string locale_;
    Language menu_preference_ = Language::system;
    Theme menu_theme_         = Theme::system;
    Theme system_theme_       = Theme::system;
    bool menu_fullscreen_     = false;
    bool chinese_font_        = false;
    bool quit_                = false;
    bool applied_dark_        = true;
    float applied_scale_      = 0.0f;
    Uint64 last_activity_     = 0;
    unsigned rendered_frames_ = 0;
    std::filesystem::path settings_path_;
    SDL_Window *window_ = nullptr;
    std::unique_ptr<render::Renderer> renderer_;
    std::unique_ptr<Viewer> viewer_;
    std::shared_ptr<DialogResult> dialog_;
    render::Image icon_;
    Uint64 smoke_started_ = 0;
    bool smoke_captured_  = false;
    bool sdl_ready_       = false;
    bool imgui_ready_     = false;
    bool platform_ready_  = false;
};
} // namespace

int run_application(const LaunchOptions &options)
{
    Application application;
    return application.run(options);
}
} // namespace liltview
