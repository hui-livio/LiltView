#include "application.hpp"
#include "logging/configure.hpp"
#include "platform/path.hpp"

#include "platform/fonts.hpp"
#include "platform/global_menu.hpp"
#include "platform/live_resize.hpp"
#include "platform/sdl_render_host.hpp"
#include "platform/window_shape.hpp"
#include "platform/window_theme.hpp"
#include "renderer/renderer.hpp"
#include "resources/icon_rgba.hpp"
#include "settings/settings.hpp"
#include "settings/store.hpp"
#include "ui/close_timing.hpp"
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
        std::string error;
        bool failed = false;
    };

public:
    ~Application()
    {
        window_shape_.reset(); // Native subclass/proxies must go before SDL's window/display connection.
        // 先撤销原生命中测试，后续窗口 / ImGui 析构期间不再访问 Application 状态。
        if (window_ != nullptr)
            SDL_SetWindowHitTest(window_, nullptr, nullptr);
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
        menu_ = current_menu(shell.image_info_open());
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
        window_shape_ = std::make_unique<WindowShape>();
        initialize_imgui();
        create_icon();
        load_font();
        update_visual_style();
        configure_hit_test();
        viewer_ = std::make_unique<Viewer>(*renderer_);
        if (!options.image_path.empty())
            viewer_->open(utf8_path(options.image_path));
        smoke_started_ = SDL_GetTicks();
        if (settings_.maximized)
            require(SDL_MaximizeWindow(window_), "SDL_MaximizeWindow");
        update_window_shape();
        if (!options.smoke_test)
            require(SDL_ShowWindow(window_), "SDL_ShowWindow");
        update_native_window_theme();
        spdlog::info("LiltView {} started, app_id={}, video={}, renderer=OpenGL3, texture_limit={}", LILTVIEW_VERSION,
                     LILTVIEW_APP_ID, SDL_GetCurrentVideoDriver(), renderer_->capabilities().max_texture_edge);
    }

    // 窗口和 GL 上下文准备完成后才创建纹理；任一阶段失败都由析构函数回收已完成部分。
    SDL_Window *make_window(int width, int height, bool system_titlebar)
    {
        const auto render_flags = prepare_render_window();
        auto *window =
            SDL_CreateWindow("LiltView", width, height,
                             render_flags | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN |
                                 (system_titlebar ? 0 : SDL_WINDOW_BORDERLESS) |
                                 (std::string_view(SDL_GetCurrentVideoDriver()) == "x11" ? 0 : SDL_WINDOW_TRANSPARENT));
        if (!window)
            return nullptr;
        if (!SDL_SetWindowMinimumSize(window, 640, 480))
        {
            SDL_DestroyWindow(window);
            return nullptr;
        }
        spdlog::debug("Window created: system_titlebar={}, transparent={}, native_window_id={}", system_titlebar,
                      (SDL_GetWindowFlags(window) & SDL_WINDOW_TRANSPARENT) != 0, SDL_GetWindowID(window));
        return window;
    }

    void create_window()
    {
        window_ = make_window(settings_.width, settings_.height, settings_.system_titlebar);
        require(window_ != nullptr, "SDL_CreateWindow");
        applied_system_titlebar_ = settings_.system_titlebar;
    }

    void configure_hit_test()
    {
        if (!SDL_SetWindowHitTest(window_, applied_system_titlebar_ ? nullptr : window_hit_test,
                                  applied_system_titlebar_ ? nullptr : this) &&
            !applied_system_titlebar_)
            spdlog::warn("Window hit testing is unavailable: {}. Enable the system titlebar if native move/resize is needed.",
                         SDL_GetError());
    }

    bool apply_window_mode(Shell &shell)
    {
        if (settings_.system_titlebar == applied_system_titlebar_)
            return false;
        if (dialog_)
        {
            settings_.system_titlebar = applied_system_titlebar_;
            spdlog::warn("Close the file dialog before switching window decorations");
            return false;
        }
        shell.dismiss_transient_menus();
        const auto flags            = SDL_GetWindowFlags(window_);
        const auto original_window  = SDL_GetWindowID(window_);
        const auto original_context = SDL_GL_GetCurrentContext();
        int width = settings_.width, height = settings_.height;
        const bool normal = (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) == 0;
        if (normal)
            SDL_GetWindowSize(window_, &width, &height);
        auto before            = current_chrome_state();
        before.system_titlebar = applied_system_titlebar_;
        before.maximized = before.fullscreen = false;
        auto after                           = before;
        after.system_titlebar                = settings_.system_titlebar;
        const auto placement                 = window_content_size_for_mode(width, height, before, after);

        // X11 keeps an opaque visual for its whole lifetime; native Shape clips
        // rounded custom corners. Remove its bounding/input masks *before*
        // asking the WM to restore a native frame around this same client.
        shape_width_ = shape_height_ = 0;
        if (settings_.system_titlebar && !window_shape_->apply(window_, 0, true))
        {
            settings_.system_titlebar = applied_system_titlebar_;
            spdlog::warn("Cannot remove custom window shape: {}", SDL_GetError());
            return false;
        }
        if (!SDL_SetWindowBordered(window_, settings_.system_titlebar))
        {
            settings_.system_titlebar = applied_system_titlebar_;
            return false;
        }
        applied_system_titlebar_ = settings_.system_titlebar;
        native_theme_dirty_      = true;
        configure_hit_test();
        if (normal)
            SDL_SetWindowSize(window_, placement.width, placement.height);
        SDL_SyncWindow(window_);
        SDL_PumpEvents();
        settings_.width  = placement.width;
        settings_.height = placement.height;
        close_inputs_    = {};
        spdlog::debug(
            "Window decoration applied: system_titlebar={}, transparent={}, native_window_id={}, surface_reused={}, gl_context_reused={}",
            settings_.system_titlebar, (SDL_GetWindowFlags(window_) & SDL_WINDOW_TRANSPARENT) != 0, SDL_GetWindowID(window_),
            original_window == SDL_GetWindowID(window_), original_context == SDL_GL_GetCurrentContext());
        return true;
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
        last_activity_         = SDL_GetTicks();
        menu_preference_       = settings_.language;
        menu_theme_            = settings_.theme;
        menu_fullscreen_       = is_fullscreen();
        menu_system_titlebar_  = settings_.system_titlebar;
        menu_disable_rounding_ = settings_.disable_rounding;
        menu_has_image_        = viewer_->has_image();
        menu_can_actual_size_  = viewer_->can_actual_size();
        while (!quit_)
        {
            dispatch_global_menu_actions(global_menu, shell);

            if (quit_)
                break;
            const bool mode_changed = apply_window_mode(shell);

            if (options.smoke_test)
                inject_smoke_events(shell);

            {
                // Windows' modal move/resize loop does not return to this outer
                // loop until release. Redraw on SDL's live expose notification,
                // not by pumping events recursively or changing native hit tests.
                LiveResizeRedraw live_resize(SDL_GetWindowID(window_), [&]
                {
                    if (quit_ || (SDL_GetWindowFlags(window_) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0)
                        return;
                    // SDL holds its event-watch lock here. Do not drain or
                    // retire image work: retirement may join a worker waiting
                    // to post an SDL wakeup. Redraw the current scene only;
                    // normal viewer pumping resumes after the native loop.
                    update_visual_style();
                    update_window_shape(); // Match the native region to this frame's current dimensions.
                    render_frame(options, shell, global_menu.active(), true);
                });
                process_events(options.smoke_test, shell, mode_changed);
                live_resize.rethrow_if_failed();
            }
            // 系统菜单动作由刚收到的 SDL 唤醒事件触发，不延后到下一帧。
            if (!quit_)
                dispatch_global_menu_actions(global_menu, shell);
            if (quit_)
                break;
            apply_window_mode(shell);

            consume_dialog();
            viewer_->pump();

            if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0)
                continue;

            update_visual_style();
            update_window_shape();
            update_native_window_theme();
            synchronize_menu(global_menu, shell);
            render_frame(options, shell, global_menu.active());
        }
    }

    void request_quit(const CloseTiming &timing = {})
    {
        if (quit_)
            return;
        const auto started = SDL_GetTicksNS();
        quit_              = true;
        // 先让窗口从桌面消失，解码、总线及 GL 仍按原有安全顺序回收。
        if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_HIDDEN) == 0)
        {
            if (!SDL_HideWindow(window_))
                spdlog::warn("Cannot hide closing window: {}", SDL_GetError());
            // Wayland 的取消映射请求需要及时 flush，不能等资源回收结束再发送。
            SDL_PumpEvents();
        }
        const auto hide_ms = static_cast<double>(SDL_GetTicksNS() - started) / 1000000.0;
        spdlog::debug("Close requested: window_hidden={}, frames={}, hide_ms={:.3f}",
                      (SDL_GetWindowFlags(window_) & SDL_WINDOW_HIDDEN) != 0, rendered_frames_, hide_ms);
        const auto event_to_handler   = close_elapsed_ms(timing.event_ns, timing.handled_ns);
        const auto handler_to_request = close_elapsed_ms(timing.handled_ns, started);
        spdlog::debug("Close latency: source={}, origin={}, event_to_handler_ms={:.3f}, handler_to_request_ms={:.3f}, "
                      "event_to_request_ms={:.3f}, timing_valid={}",
                      timing.source, timing.origin, event_to_handler, handler_to_request,
                      close_elapsed_ms(timing.event_ns, started), event_to_handler >= 0 && handler_to_request >= 0);
    }

    // 系统菜单、窗口菜单和快捷键统一分发，避免操作行为不一致。
    void dispatch_menu_action(MenuAction action, Shell &shell, const CloseTiming &timing = {})
    {
        apply_menu_setting(action, settings_);
        switch (action)
        {
        case MenuAction::quit:
        case MenuAction::close_window:
            request_quit(timing);
            break;
        case MenuAction::disable_rounding:
            break;
        case MenuAction::system_titlebar:
            // Window transparency is a creation-time property. Commit the mode
            // change at the next frame boundary, never inside an ImGui frame.
            break;
        case MenuAction::minimize:
            if (!SDL_MinimizeWindow(window_))
                spdlog::warn("Cannot minimize window: {}", SDL_GetError());
            break;
        case MenuAction::maximize_restore:
            if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0)
            {
                if (!SDL_RestoreWindow(window_))
                    spdlog::warn("Cannot restore window: {}", SDL_GetError());
            }
            else if (!SDL_MaximizeWindow(window_))
                spdlog::warn("Cannot maximize window: {}", SDL_GetError());
            break;
        case MenuAction::fullscreen:
            toggle_fullscreen();
            break;
        case MenuAction::about:
            shell.show_about();
            break;
        case MenuAction::image_info:
            if (viewer_->has_visible_image() || shell.image_info_open())
            {
                shell.toggle_image_info();
                spdlog::debug("Image information panel: open={}", shell.image_info_open());
            }
            break;
        case MenuAction::open:
            open_dialog();
            break;
        case MenuAction::previous:
            viewer_->navigate(-1);
            break;
        case MenuAction::next:
            viewer_->navigate(1);
            break;
        case MenuAction::fit:
            if (viewer_->has_image())
                viewer_->fit();
            break;
        case MenuAction::actual_size:
            if (viewer_->has_image() && viewer_->can_actual_size())
                viewer_->actual_size();
            break;
        case MenuAction::pan_left:
        case MenuAction::pan_right:
        case MenuAction::pan_up:
        case MenuAction::pan_down:
            if (viewer_->has_image())
            {
                const auto delta = menu_pan_delta(action);
                viewer_->pan(delta[0], delta[1]);
            }
            break;
        case MenuAction::rotate_left:
        case MenuAction::rotate_right:
            if (viewer_->has_image())
                viewer_->rotate(action == MenuAction::rotate_left ? -1 : 1);
            break;
        case MenuAction::zoom_in:
            if (viewer_->has_image())
                viewer_->zoom(1);
            break;
        case MenuAction::zoom_out:
            if (viewer_->has_image())
                viewer_->zoom(-1);
            break;
        default:
            break;
        }
    }

    void dispatch_global_menu_actions(GlobalMenu &global_menu, Shell &shell)
    {
        for (const auto &activation : global_menu.take_actions())
        {
            const CloseTiming timing{"global_menu", "dbus_activation", activation.received_ns, SDL_GetTicksNS()};
            dispatch_menu_action(activation.action, shell, timing);
            if (quit_)
                break;
        }
    }

    void stop_image_work()
    {
        spdlog::debug("Shutdown: stopping image work, window_hidden={}, frames={}",
                      (SDL_GetWindowFlags(window_) & SDL_WINDOW_HIDDEN) != 0, rendered_frames_);
        viewer_->begin_shutdown();
        // 等核心的退出条件变量，避免末次唤醒早于 exited 标志而额外等 10ms。
        // 等待期间仍定期处理平台通知，但不再加载文件、渲染或分发菜单动作。
        while (!viewer_->join_shutdown(10))
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
                ImGui_ImplSDL3_ProcessEvent(&event);
        }
    }

    // 最近有交互时保持响应；静止时延长等待，避免持续空转重绘。
    void process_events(bool smoke_test, Shell &shell, bool force_poll)
    {
        const bool active = viewer_->loading() || viewer_->detail_loading() || shell.needs_animation_frame() ||
            SDL_GetTicks() - last_activity_ < 180 || ImGui::IsAnyItemActive();
        const auto flags = SDL_GetWindowFlags(window_);
        viewer_->suspend_animation(!smoke_test && (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)));
        const int wait_ms = (smoke_test || force_poll) ? 0 : viewer_->wait_timeout_ms(active ? 16 : 200);
        SDL_Event event;
        if (!SDL_WaitEventTimeout(&event, wait_ms))
            return;
        do
        {
            handle_event(event, shell);
        } while (!quit_ && SDL_PollEvent(&event));
    }

    void handle_event(const SDL_Event &event, Shell &shell)
    {
        const auto handled_ns  = SDL_GetTicksNS();
        const bool trace_input = spdlog::should_log(spdlog::level::debug);
        auto &context          = *ImGui::GetCurrentContext();
        if (trace_input)
            close_inputs_.begin_event(context);
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (trace_input)
            close_inputs_.end_event(context, event.common.timestamp, handled_ns);
        last_activity_ = SDL_GetTicks();
        if (event.type == SDL_EVENT_SYSTEM_THEME_CHANGED)
            refresh_system_theme();
        if (event.type == SDL_EVENT_QUIT ||
            (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window_)))
        {
            request_quit(
                {event.type == SDL_EVENT_QUIT ? "sdl_quit" : "window_close", "sdl_event", event.common.timestamp, handled_ns});
            return;
        }
        if (event.type == SDL_EVENT_KEY_DOWN)
            handle_shortcut(event.key, shell, {"shortcut", "sdl_key_down", event.common.timestamp, handled_ns});
        if (event.type == SDL_EVENT_DROP_FILE && event.drop.data)
            viewer_->open(utf8_path(event.drop.data));
        update_window_settings(event);
    }

    void handle_shortcut(const SDL_KeyboardEvent &key, Shell &shell, const CloseTiming &timing)
    {
        // Dock keyboard navigation consumes plain arrows/activation/Escape before
        // the image shortcuts; modified image shortcuts still use the menu model.
        const bool dock_key = shell.dock_keyboard_focus() && (key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0 &&
            (key.key == SDLK_LEFT || key.key == SDLK_RIGHT || key.key == SDLK_UP || key.key == SDLK_DOWN || key.key == SDLK_TAB ||
             key.key == SDLK_RETURN || key.key == SDLK_SPACE || key.key == SDLK_ESCAPE);
        // 文本输入和弹窗优先处理按键；图片尚未加载完成时不执行应用快捷键。
        const ShortcutContext context{dialog_ || dock_key || ImGui::GetIO().WantTextInput ||
                                          ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel),
                                      is_fullscreen(), shell.image_info_open(), !viewer_->settled()};
        // 使用菜单模型中的同一份快捷键定义；禁用菜单项不会响应按键。
        std::vector<std::string> shortcut;
        if ((key.mod & SDL_KMOD_CTRL) != 0)
            shortcut.emplace_back("Control");
        if ((key.mod & SDL_KMOD_SHIFT) != 0)
            shortcut.emplace_back("Shift");
        if ((key.mod & SDL_KMOD_ALT) != 0)
            shortcut.emplace_back("Alt");
        if ((key.mod & SDL_KMOD_GUI) != 0)
            shortcut.emplace_back("Super");
        if (key.key >= SDLK_A && key.key <= SDLK_Z)
            shortcut.emplace_back(1, static_cast<char>('a' + key.key - SDLK_A));
        else
            shortcut.emplace_back(SDL_GetKeyName(key.key));
        dispatch_menu_action(menu_action_for_input(current_menu(shell.image_info_open()), shortcut, key.repeat, context), shell,
                             timing);
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
            // SDL errors belong to the callback thread; save them before returning.
            if (result->failed)
                result->error = SDL_GetError();
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
        static constexpr SDL_DialogFileFilter filters[]{
            {"Images", "jpg;jpeg;png;svg;webp;bmp;gif;tif;tiff;JPG;JPEG;PNG;SVG;WEBP;BMP;GIF;TIF;TIFF"}, {"All files", "*"}};
        auto *reference = new std::shared_ptr<DialogResult>(dialog_);
        SDL_ShowOpenFileDialog(dialog_callback, reference, window_, filters, 2, nullptr, false);
    }

    void consume_dialog()
    {
        if (!dialog_ || !dialog_->ready.load())
            return;
        std::string path;
        std::string error;
        {
            std::lock_guard lock(dialog_->mutex);
            path  = dialog_->path;
            error = dialog_->error;
        }
        if (dialog_->failed)
            spdlog::warn("File dialog failed: {}", error.empty() ? "unknown SDL error" : error);
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
            applied_scale_      = scale;
            applied_dark_       = dark;
            native_theme_dirty_ = true;
        }
    }

    // Apply after showing the window and after native border/region changes.
    void update_native_window_theme()
    {
        if (native_theme_dirty_)
        {
            const bool dark = resolve_theme(settings_.theme, system_theme_) == Theme::dark;
            if (!apply_native_window_theme(window_, dark))
                spdlog::debug("Native window theme unchanged: {}", SDL_GetError());
            native_theme_dirty_ = false;
        }
    }

    // SDL 在桌面主题变化时发事件；仅更新观测值，不覆盖用户持久化的三态选择。
    void refresh_system_theme()
    {
        native_theme_dirty_ = true; // SDL may reset native colors even with an explicit app theme.
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

    WindowChromeState current_chrome_state() const
    {
        WindowChromeState state;
        const auto flags      = SDL_GetWindowFlags(window_);
        state.system_titlebar = settings_.system_titlebar;
        state.rounded         = !settings_.disable_rounding;
        state.fullscreen      = (flags & SDL_WINDOW_FULLSCREEN) != 0;
        state.maximized       = (flags & SDL_WINDOW_MAXIMIZED) != 0;
        state.blocked         = dialog_ != nullptr ||
            (imgui_ready_ && ImGui::GetCurrentContext() && ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId));
        state.dark        = applied_dark_;
        state.scale       = std::max(applied_scale_, 1.0f);
        state.line_height = ImGui::GetStyle().FontSizeBase * state.scale;
        return state;
    }

    static SDL_HitTestResult SDLCALL window_hit_test(SDL_Window *window, const SDL_Point *point, void *data)
    {
        const auto &app = *static_cast<Application *>(data);
        if (app.quit_)
            return SDL_HITTEST_NORMAL;
        int width = 0, height = 0;
        if (!SDL_GetWindowSize(window, &width, &height))
            return SDL_HITTEST_NORMAL;
        const auto chrome =
            window_chrome_metrics({static_cast<float>(width), static_cast<float>(height)}, app.current_chrome_state());
        switch (window_chrome_hit(chrome, {static_cast<float>(point->x), static_cast<float>(point->y)}))
        {
        case WindowHit::drag:
            return SDL_HITTEST_DRAGGABLE;
        case WindowHit::top_left:
            return SDL_HITTEST_RESIZE_TOPLEFT;
        case WindowHit::top:
            return SDL_HITTEST_RESIZE_TOP;
        case WindowHit::top_right:
            return SDL_HITTEST_RESIZE_TOPRIGHT;
        case WindowHit::right:
            return SDL_HITTEST_RESIZE_RIGHT;
        case WindowHit::bottom_right:
            return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        case WindowHit::bottom:
            return SDL_HITTEST_RESIZE_BOTTOM;
        case WindowHit::bottom_left:
            return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        case WindowHit::left:
            return SDL_HITTEST_RESIZE_LEFT;
        default:
            return SDL_HITTEST_NORMAL;
        }
    }

    void update_window_shape()
    {
        int width = 0, height = 0;
        if (!SDL_GetWindowSize(window_, &width, &height))
            return;
        const float density = SDL_GetWindowPixelDensity(window_);
        const float radius =
            window_chrome_metrics({static_cast<float>(width), static_cast<float>(height)}, current_chrome_state()).radius;
        if (shape_width_ == width && shape_height_ == height && shape_radius_ == radius && shape_density_ == density)
            return;
        if (!window_shape_->apply(window_, radius, settings_.system_titlebar))
        {
            spdlog::warn("Cannot update rounded window shape: {}", SDL_GetError());
            if (radius > 0 && std::string_view(SDL_GetCurrentVideoDriver()) == "x11")
            {
                // An opaque X11 visual needs Shape for clipped corners. Never
                // leave un-clipped black triangles when the extension is absent.
                settings_.disable_rounding = true;
                spdlog::warn("Rounded corners disabled because native X11 shape is unavailable");
            }
        }
        native_theme_dirty_ = true;
        shape_width_        = width;
        shape_height_       = height;
        shape_radius_       = radius;
        shape_density_      = density;
    }

    MenuItem current_menu(bool image_info_open) const
    {
        return build_menu(settings_, resolve_language(settings_.language, locale_, chinese_font_), chinese_font_, is_fullscreen(),
                          viewer_->has_image(), viewer_->can_actual_size(), viewer_->navigation().can_previous,
                          viewer_->navigation().can_next, image_info_open, viewer_->has_visible_image());
    }

    // 语言、主题、全屏或图片可用状态变化时，同步系统顶栏和窗口回退菜单。
    void synchronize_menu(GlobalMenu &global_menu, const Shell &shell)
    {
        if (menu_preference_ == settings_.language && menu_theme_ == settings_.theme && menu_fullscreen_ == is_fullscreen() &&
            menu_has_image_ == viewer_->has_image() && menu_can_actual_size_ == viewer_->can_actual_size() &&
            menu_can_previous_ == viewer_->navigation().can_previous && menu_can_next_ == viewer_->navigation().can_next &&
            menu_image_info_open_ == shell.image_info_open() && menu_has_image_info_ == viewer_->has_visible_image() &&
            menu_system_titlebar_ == settings_.system_titlebar && menu_disable_rounding_ == settings_.disable_rounding)
            return;
        menu_ = current_menu(shell.image_info_open());
        global_menu.update(menu_);
        menu_preference_       = settings_.language;
        menu_theme_            = settings_.theme;
        menu_fullscreen_       = is_fullscreen();
        menu_system_titlebar_  = settings_.system_titlebar;
        menu_disable_rounding_ = settings_.disable_rounding;
        menu_has_image_        = viewer_->has_image();
        menu_can_actual_size_  = viewer_->can_actual_size();
        menu_can_previous_     = viewer_->navigation().can_previous;
        menu_can_next_         = viewer_->navigation().can_next;
        menu_image_info_open_  = shell.image_info_open();
        menu_has_image_info_   = viewer_->has_visible_image();
    }

    void render_frame(const LaunchOptions &options, Shell &shell, bool system_menu, bool live_resize = false)
    {
        renderer_->begin_frame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const auto language = resolve_language(settings_.language, locale_, chinese_font_);
        const auto action =
            shell.draw(language, menu_, system_menu, *renderer_, icon_, *viewer_, current_chrome_state(), dialog_ != nullptr);
        if (live_resize)
        {
            // Only present during the native loop. Window/menu actions, dialogs
            // and mode switches stay on the normal outer-loop dispatch path.
            ImGui::Render();
            present_frame(options, false);
            ++rendered_frames_;
            return;
        }
        auto &context       = *ImGui::GetCurrentContext();
        auto timing = (action == MenuAction::quit || action == MenuAction::close_window) ? close_inputs_.activation(context) :
                                                                                           CloseTiming{};
        if (action == MenuAction::close_window)
            timing.source = "titlebar_button";
        close_inputs_.discard_processed(context);
        dispatch_menu_action(action, shell, timing);
        if (quit_ || action == MenuAction::system_titlebar || action == MenuAction::disable_rounding)
        {
            // 关闭 / 切换装饰后不提交旧布局，下一帧使用新的标题栏状态。
            // 菜单退出不再提交最后一帧，避免等待 present/VSync。
            ImGui::EndFrame();
            return;
        }
        ImGui::Render();
        present_frame(options);
        ++rendered_frames_;
    }

    void present_frame(const LaunchOptions &options, bool capture = true)
    {
        const auto border = window_border_color();
        renderer_->set_opaque_background({border.x, border.y, border.z});
        renderer_->render_frame(ImGui::GetDrawData());
        if (capture)
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
            const auto filename  = utf8_path(options.screenshot);
            auto about           = filename.parent_path() / filename.stem();
            about               += "-about.ppm";
            save_screenshot(platform::path_bytes(about));
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

    void create_window_icon()
    {
        auto *surface = SDL_CreateSurfaceFrom(resources::icon_size, resources::icon_size, SDL_PIXELFORMAT_RGBA32,
                                              const_cast<unsigned char *>(resources::icon_rgba.data()), resources::icon_size * 4);
        if (surface != nullptr)
        {
            if (!SDL_SetWindowIcon(window_, surface))
                spdlog::debug("Window icon: {}", SDL_GetError());
            SDL_DestroySurface(surface);
        }
    }

    void create_icon()
    {
        create_window_icon();
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
        if (!pixels.alpha.empty())
        {
            const auto image  = utf8_path(filename);
            auto alpha_path   = image.parent_path() / image.stem();
            alpha_path       += "-alpha.pgm";
            std::ofstream alpha(alpha_path, std::ios::binary);
            alpha << "P5\n" << pixels.size.width << ' ' << pixels.size.height << "\n255\n";
            alpha.write(reinterpret_cast<const char *>(pixels.alpha.data()), static_cast<std::streamsize>(pixels.alpha.size()));
            if (!alpha)
                throw std::runtime_error("Cannot save screenshot alpha plane");
        }
    }

    // 应用状态只在主线程读写；GlobalMenu 在 run() 内先销毁，再回收这些图形资源。
    Settings settings_;
    UiCloseTiming close_inputs_;
    MenuItem menu_;
    std::string locale_;
    Language menu_preference_     = Language::system;
    Theme menu_theme_             = Theme::system;
    Theme system_theme_           = Theme::system;
    bool menu_fullscreen_         = false;
    bool menu_system_titlebar_    = false;
    bool applied_system_titlebar_ = false;
    bool native_theme_dirty_      = true;
    bool menu_disable_rounding_   = false;
    int shape_width_ = 0, shape_height_ = 0;
    float shape_radius_ = -1, shape_density_ = 0;
    std::unique_ptr<WindowShape> window_shape_;
    bool menu_has_image_       = false;
    bool menu_can_actual_size_ = false;
    bool menu_can_previous_    = false;
    bool menu_can_next_        = false;
    bool menu_image_info_open_ = false;
    bool menu_has_image_info_  = false;
    bool chinese_font_         = false;
    bool quit_                 = false;
    bool applied_dark_         = true;
    float applied_scale_       = 0.0f;
    Uint64 last_activity_      = 0;
    unsigned rendered_frames_  = 0;
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
