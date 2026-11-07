#include "application.hpp"

#include "platform/fonts.hpp"
#include "platform/global_menu.hpp"
#include "resources/icon_rgba.hpp"
#include "settings/settings.hpp"
#include "settings/store.hpp"
#include "ui/shell.hpp"
#include "version.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
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
public:
    ~Application()
    {
        // 逆初始化顺序释放；标志位也覆盖启动途中抛异常的情况。
        if (renderer_ready_)
            ImGui_ImplOpenGL3_Shutdown();
        if (platform_ready_)
            ImGui_ImplSDL3_Shutdown();
        if (imgui_ready_)
            ImGui::DestroyContext();
        if (icon_texture_ != 0)
            glDeleteTextures(1, &icon_texture_);
        if (context_ != nullptr)
            SDL_GL_DestroyContext(context_);
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
        locale_ = system_locale();
        if (options.language)
            settings_.language = *options.language;

        create_window();
        initialize_imgui();
        load_font();
        if (settings_.maximized)
            require(SDL_MaximizeWindow(window_), "SDL_MaximizeWindow");
        if (!options.smoke_test)
            require(SDL_ShowWindow(window_), "SDL_ShowWindow");
        spdlog::info("LiltView {} started, app_id={}, video={}, OpenGL={}", LILTVIEW_VERSION, LILTVIEW_APP_ID,
                     SDL_GetCurrentVideoDriver(), reinterpret_cast<const char *>(glGetString(GL_VERSION)));
    }

    // 窗口和 GL 上下文准备完成后才创建纹理；任一阶段失败都由析构函数回收已完成部分。
    void create_window()
    {
        require(SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE), "GL profile");
        require(SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3), "GL major version");
        require(SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3), "GL minor version");
        require(SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1), "GL double buffer");
        window_ = SDL_CreateWindow("LiltView", settings_.width, settings_.height,
                                   SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
        require(window_ != nullptr, "SDL_CreateWindow");
        require(SDL_SetWindowMinimumSize(window_, 640, 480), "SDL_SetWindowMinimumSize");
        context_ = SDL_GL_CreateContext(window_);
        require(context_ != nullptr, "SDL_GL_CreateContext");
        require(SDL_GL_MakeCurrent(window_, context_), "SDL_GL_MakeCurrent");
        if (!SDL_GL_SetSwapInterval(1))
            spdlog::warn("VSync unavailable: {}", SDL_GetError());
        create_icon();
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
        platform_ready_ = ImGui_ImplSDL3_InitForOpenGL(window_, context_);
        require(platform_ready_, "ImGui SDL3 backend");
        renderer_ready_ = ImGui_ImplOpenGL3_Init("#version 330 core");
        require(renderer_ready_, "ImGui OpenGL backend");
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
        }
    }

    // 最近有交互时保持响应；静止时延长等待，避免持续空转重绘。
    void process_events(bool smoke_test)
    {
        const bool active = SDL_GetTicks() - last_activity_ < 180 || ImGui::IsAnyItemActive();
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
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const auto language = resolve_language(settings_.language, locale_, chinese_font_);
        const auto actions =
            shell.draw(settings_, language, menu_, system_menu, is_fullscreen(), static_cast<ImTextureID>(icon_texture_));
        if (actions.quit)
            quit_ = true;
        if (actions.toggle_fullscreen)
            toggle_fullscreen();
        ImGui::Render();
        present_frame(options);
        ++rendered_frames_;
    }

    void present_frame(const LaunchOptions &options)
    {
        int width  = 0;
        int height = 0;
        require(SDL_GetWindowSizeInPixels(window_, &width, &height), "Framebuffer dimensions");
        if (width <= 0 || height <= 0)
            return;
        glViewport(0, 0, width, height);
        glClearColor(0.055f, 0.063f, 0.075f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        capture_smoke_frame(options, width, height);
        require(SDL_GL_SwapWindow(window_), "SDL_GL_SwapWindow");
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
        else if (rendered_frames_ == 11)
        {
            event.type            = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
            event.window.windowID = SDL_GetWindowID(window_);
        }
        if (event.type != 0)
            SDL_PushEvent(&event);
    }

    void capture_smoke_frame(const LaunchOptions &options, int width, int height)
    {
        if (!options.smoke_test || options.screenshot.empty())
            return;
        if (rendered_frames_ == 5)
        {
            const auto filename = utf8_path(options.screenshot);
            const auto about    = filename.parent_path() / (filename.stem().string() + "-about.ppm");
            save_screenshot(about.string(), width, height);
        }
        if (rendered_frames_ == 10)
            save_screenshot(options.screenshot, width, height);
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
            settings_path_ = utf8_path(options.config_directory) / "settings.ini";
        else if (!options.smoke_test)
        {
            char *directory = SDL_GetPrefPath("LiltView", "LiltView");
            if (directory != nullptr)
            {
                settings_path_ = utf8_path(directory) / "settings.ini";
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
        glGenTextures(1, &icon_texture_);
        glBindTexture(GL_TEXTURE_2D, icon_texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, resources::icon_size, resources::icon_size, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     resources::icon_rgba.data());
        // 高分辨率源图覆盖常见 2x / 3x 显示；mipmap 让较小尺寸也能稳定、清晰地采样。
        const auto generate_mipmap = reinterpret_cast<PFNGLGENERATEMIPMAPPROC>(SDL_GL_GetProcAddress("glGenerateMipmap"));
        require(generate_mipmap != nullptr, "glGenerateMipmap");
        generate_mipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (icon_texture_ == 0 || glGetError() != GL_NO_ERROR)
            throw std::runtime_error("Cannot upload application icon");
    }

    void save_screenshot(const std::string &filename, int width, int height)
    {
        const auto row_bytes = static_cast<std::size_t>(width) * 3;
        std::vector<unsigned char> pixels(row_bytes * static_cast<std::size_t>(height));
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
        if (glGetError() != GL_NO_ERROR)
            throw std::runtime_error("Screenshot readback failed");
        std::ofstream output(utf8_path(filename), std::ios::binary);
        output << "P6\n" << width << ' ' << height << "\n255\n";
        for (int y = height; y-- > 0;)
            output.write(reinterpret_cast<const char *>(pixels.data() + static_cast<std::size_t>(y) * row_bytes),
                         static_cast<std::streamsize>(row_bytes));
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
    SDL_Window *window_    = nullptr;
    SDL_GLContext context_ = nullptr;
    GLuint icon_texture_   = 0;
    bool sdl_ready_        = false;
    bool imgui_ready_      = false;
    bool platform_ready_   = false;
    bool renderer_ready_   = false;
};
} // namespace

int run_application(const LaunchOptions &options)
{
    Application application;
    return application.run(options);
}
} // namespace liltview
