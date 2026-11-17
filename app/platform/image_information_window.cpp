#include "image_information_window.hpp"
#include "renderer/renderer.hpp"
#include "sdl_render_host.hpp"
#include "ui/image_information.hpp"
#include "ui/shell.hpp"
#include "window_theme.hpp"

#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <stdexcept>
#include <string>

namespace liltview
{
namespace
{
struct ContextScope
{
    ImGuiContext *previous = ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext *context)
    {
        ImGui::SetCurrentContext(context);
    }
    ~ContextScope()
    {
        ImGui::SetCurrentContext(previous);
    }
};
void check(bool ok, const char *operation)
{
    if (!ok)
        throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}
} // namespace

ImageInformationWindow::ImageInformationWindow(SDL_Window *main_window, const SystemFont &font)
{
    // CreateContext may select the new context when the caller has none.
    // Save it before creation, not only before initializing the backends.
    const ContextScope restore(ImGui::GetCurrentContext());
    try
    {
        window_ = SDL_CreateWindow("Image information - LiltView", 380, 560,
                                   SDL_WINDOW_UTILITY | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
        check(window_ != nullptr, "Create image information window");
        check(SDL_SetWindowMinimumSize(window_, 260, 240), "Set image information minimum size");
        if (main_window)
        {
            const auto display = SDL_GetDisplayForWindow(main_window);
            if (display)
                SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        }
        context_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(context_);
        auto &io       = ImGui::GetIO();
        io.IniFilename = io.LogFilename  = nullptr;
        io.ConfigFlags                  |= ImGuiConfigFlags_NavEnableKeyboard;
        if (font.path.empty() || !load_font_candidate(font))
            io.Fonts->AddFontDefault();
        // Never share an atlas: its backend texture IDs belong to one renderer.
        renderer_       = render::create_renderer({render::SurfaceKind::sdl_window, window_, 4ULL * 1024 * 1024, false});
        platform_ready_ = initialize_imgui_platform(window_);
        check(platform_ready_, "Initialize image information platform");
    } catch (...)
    {
        release();
        throw;
    }
}

ImageInformationWindow::~ImageInformationWindow()
{
    release();
}

void ImageInformationWindow::release() noexcept
{
    ContextScope scope(context_);
    if (scope.previous == context_)
        scope.previous = nullptr;
    renderer_.reset();
    if (platform_ready_)
        ImGui_ImplSDL3_Shutdown();
    platform_ready_ = false;
    if (context_)
        ImGui::DestroyContext(context_);
    context_ = nullptr;
    if (window_)
        SDL_DestroyWindow(window_);
    window_ = nullptr;
}

SDL_Window *ImageInformationWindow::window() const noexcept
{
    return window_;
}

bool ImageInformationWindow::visible() const noexcept
{
    return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_HIDDEN) == 0;
}

void ImageInformationWindow::show()
{
    if (visible())
        return;
    check(SDL_ShowWindow(window_), "Show image information window");
    if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0)
        SDL_RestoreWindow(window_);
    // Keep the existing desktop placement and size when reopening.
    SDL_RaiseWindow(window_); // Best effort: some compositors deny focus stealing.
}

void ImageInformationWindow::hide() noexcept
{
    if (window_)
        SDL_HideWindow(window_);
}

bool ImageInformationWindow::process_event(const SDL_Event &event)
{
    if (SDL_GetWindowFromEvent(&event) != window_)
        return false;
    const ContextScope scope(context_);
    ImGui_ImplSDL3_ProcessEvent(&event);
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        hide();
    else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
             (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0 &&
             (event.key.key == SDLK_I || event.key.key == SDLK_ESCAPE) && !ImGui::GetIO().WantTextInput)
        hide();
    return true;
}

void ImageInformationWindow::draw(Language language, const std::optional<ImageDetails> &details, bool dark)
{
    if (!visible() || (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0)
        return;
    const ContextScope scope(context_);
    const float density = std::max(SDL_GetWindowPixelDensity(window_), 1.0f);
    const float scale   = std::clamp(SDL_GetWindowDisplayScale(window_) / density, 1.0f, 4.0f);
    if (std::abs(scale - scale_) > 0.01f || dark != dark_)
    {
        apply_style(dark, scale);
        apply_native_window_theme(window_, dark);
        dark_  = dark;
        scale_ = scale;
    }
    if (language != language_)
    {
        const auto title = std::string(translate(language, Text::image_info)) + " - LiltView";
        check(SDL_SetWindowTitle(window_, title.c_str()), "Set image information title");
        language_ = language;
    }
    renderer_->begin_frame();
    new_imgui_platform_frame(window_);
    ImGui::NewFrame();
    const auto *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("image_information_root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    draw_image_information(language, details, viewport->Pos,
                           {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y});
    ImGui::End();
    ImGui::Render();
    renderer_->render_frame(ImGui::GetDrawData());
    renderer_->present();
}
} // namespace liltview
