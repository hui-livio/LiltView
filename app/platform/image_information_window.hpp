#ifndef LILTVIEW_IMAGE_INFORMATION_WINDOW_HPP
#define LILTVIEW_IMAGE_INFORMATION_WINDOW_HPP

#include "fonts.hpp"
#include "i18n/translator.hpp"
#include "viewer/image_details.hpp"

#include <SDL3/SDL.h>
#include <memory>

struct ImGuiContext;
namespace liltview::render
{
class Renderer;
}
namespace liltview
{
// A non-modal, native desktop tool window. It has its own ImGui context,
// font atlas and renderer; no main-window layout or image resources are shared.
// All operations run on the SDL/UI thread and restore the caller's context.
class ImageInformationWindow
{
public:
    ImageInformationWindow(SDL_Window *main_window, const SystemFont &font);
    ~ImageInformationWindow();
    ImageInformationWindow(const ImageInformationWindow &)            = delete;
    ImageInformationWindow &operator=(const ImageInformationWindow &) = delete;

    SDL_Window *window() const noexcept;
    bool visible() const noexcept;
    void show();
    void hide() noexcept;
    // True only for this window's events. Close, Escape and unmodified I hide
    // only this panel; their events must not reach the main image shortcuts.
    bool process_event(const SDL_Event &);
    void draw(Language, const std::optional<ImageDetails> &, bool dark);

private:
    void release() noexcept;
    SDL_Window *window_    = nullptr;
    ImGuiContext *context_ = nullptr;
    std::unique_ptr<render::Renderer> renderer_;
    bool platform_ready_ = false;
    bool dark_           = false;
    float scale_         = 0;
    Language language_   = Language::system;
};
} // namespace liltview
#endif
