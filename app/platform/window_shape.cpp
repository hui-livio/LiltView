#include "window_shape.hpp"
#include <SDL3/SDL.h>
#include <cstring>
#include <string_view>
#include "ui/window_chrome.hpp"

#ifdef _WIN32
    #include <windows.h>

    #include <commctrl.h>
#endif

#ifdef __linux__
    #include <X11/Xlib.h>
    #include <X11/extensions/shape.h>
    #include <wayland-client.h>
#endif

namespace liltview
{
struct WindowShape::Impl
{
#ifdef _WIN32
    HWND hwnd = nullptr;
    SDL_Window *window = nullptr;
    ~Impl()
    {
        if (hwnd)
            RemoveWindowSubclass(hwnd, window_proc, reinterpret_cast<UINT_PTR>(this));
    }
    bool detach()
    {
        if (!hwnd)
            return true;
        if (!RemoveWindowSubclass(hwnd, window_proc, reinterpret_cast<UINT_PTR>(this)))
            return SDL_SetError("Cannot remove custom window frame guard");
        hwnd = nullptr;
        window = nullptr;
        return true;
    }
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                                        DWORD_PTR reference)
    {
        auto *self = reinterpret_cast<Impl *>(reference);
        if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass(hwnd, window_proc, id);
            self->hwnd = nullptr;
            self->window = nullptr;
        }
        else if ((SDL_GetWindowFlags(self->window) & (SDL_WINDOW_BORDERLESS | SDL_WINDOW_FULLSCREEN)) != 0)
        {
            // SDL keeps WS_CAPTION for native move/resize/taskbar behavior.
            // A rounded HRGN disables DWM NC rendering, so DefWindowProc may
            // otherwise paint a CLASSIC caption on the first activation/drag.
            if (message == WM_NCPAINT)
                return 0;
            if (message == WM_NCACTIVATE)
                // Keep SDL's focus bookkeeping; -1 only suppresses NC repaint.
                return DefSubclassProc(hwnd, message, wparam, -1);
        }
        // In particular, do not replace NC hit testing, sizing, move loops or
        // native-frame painting. SDL/Windows retain their existing behavior.
        return DefSubclassProc(hwnd, message, wparam, lparam);
    }
    bool attach(SDL_Window *target, HWND native)
    {
        if (hwnd == native)
            return true;
        if (hwnd)
            return SDL_SetError("WindowShape cannot be shared by multiple native windows");
        window = target;
        if (!SetWindowSubclass(native, window_proc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)))
        {
            window = nullptr;
            return SDL_SetError("Cannot install custom window frame guard: %lu", GetLastError());
        }
        hwnd = native;
        return true;
    }
#endif
#ifdef __linux__
    wl_display *display       = nullptr;
    wl_event_queue *queue     = nullptr;
    wl_registry *registry     = nullptr;
    wl_compositor *compositor = nullptr;
    ~Impl()
    {
        if (compositor)
            wl_compositor_destroy(compositor);
        if (registry)
            wl_registry_destroy(registry);
        if (queue)
            wl_event_queue_destroy(queue);
    }
    bool connect(wl_display *connection)
    {
        if (compositor)
            return true;
        if (registry)
            return false;
        display  = connection;
        queue    = wl_display_create_queue(display);
        registry = wl_display_get_registry(display);
        if (!queue || !registry)
            return false;
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(registry), queue);
        static const wl_registry_listener listener{
            [](void *data, wl_registry *reg, uint32_t name, const char *interface, uint32_t)
            {
                auto *self = static_cast<Impl *>(data);
                if (std::strcmp(interface, wl_compositor_interface.name) == 0)
                    self->compositor = static_cast<wl_compositor *>(wl_registry_bind(reg, name, &wl_compositor_interface, 1));
            },
            [](void *, wl_registry *, uint32_t) {}};
        wl_registry_add_listener(registry, &listener, this);
        return wl_display_roundtrip_queue(display, queue) >= 0 && compositor;
    }
#endif
};

WindowShape::WindowShape() :
    impl_(std::make_unique<Impl>())
{
}
WindowShape::~WindowShape() = default;

bool WindowShape::apply(SDL_Window *window, float radius, bool opaque)
{
    int width = 0, height = 0;
    if (!SDL_GetWindowSize(window, &width, &height))
        return false;
#ifdef __linux__
    const auto props = SDL_GetWindowProperties(window);
    if (auto *display =
            static_cast<wl_display *>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr)))
    {
        auto *surface =
            static_cast<wl_surface *>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
        if (!surface)
            return SDL_SetError("Missing Wayland surface for rounded window");
        if (!impl_->connect(display))
            return SDL_SetError("Wayland compositor unavailable for window regions");
        if (opaque)
        {
            auto *region = wl_compositor_create_region(impl_->compositor);
            if (!region)
                return SDL_SetError("Cannot create opaque Wayland region");
            wl_region_add(region, 0, 0, width, height);
            wl_surface_set_opaque_region(surface, region);
            wl_region_destroy(region);
        }
        else
            wl_surface_set_opaque_region(surface, nullptr);
        if (radius == 0)
            wl_surface_set_input_region(surface, nullptr);
        else
        {
            if (!impl_->connect(display))
                return SDL_SetError("Wayland compositor unavailable for rounded input region");
            auto *region = wl_compositor_create_region(impl_->compositor);
            if (!region)
                return SDL_SetError("Cannot create rounded Wayland input region");
            for (const auto &r : rounded_window_spans(width, height, radius))
                wl_region_add(region, r.x, r.y, r.width, r.height);
            wl_surface_set_input_region(surface, region);
            wl_region_destroy(region);
        }
        // The next EGL swap commits this region together with the matching alpha.
        return true;
    }
    if (auto *display = static_cast<Display *>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr)))
    {
        const auto native = static_cast<Window>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
        if (!native)
            return SDL_SetError("Missing X11 window for rounded window");
        int event_base = 0, error_base = 0;
        if (!XShapeQueryExtension(display, &event_base, &error_base))
            return radius == 0 ? true : SDL_SetError("X11 Shape extension unavailable");
        if (radius == 0)
        {
            XShapeCombineMask(display, native, ShapeBounding, 0, 0, None, ShapeSet);
            XShapeCombineMask(display, native, ShapeInput, 0, 0, None, ShapeSet);
        }
        else
        {
            int pixel_width = 0, pixel_height = 0;
            if (!SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height) || pixel_width > 32767 || pixel_height > 32767)
                return SDL_SetError("Unsupported X11 shape dimensions");
            const auto spans = rounded_window_spans(pixel_width, pixel_height, radius * SDL_GetWindowPixelDensity(window));
            std::vector<XRectangle> rectangles;
            rectangles.reserve(spans.size());
            for (const auto &r : spans)
                rectangles.push_back({static_cast<short>(r.x), static_cast<short>(r.y), static_cast<unsigned short>(r.width),
                                      static_cast<unsigned short>(r.height)});
            XShapeCombineRectangles(display, native, ShapeBounding, 0, 0, rectangles.data(), static_cast<int>(rectangles.size()),
                                    ShapeSet, Unsorted);
            XShapeCombineRectangles(display, native, ShapeInput, 0, 0, rectangles.data(), static_cast<int>(rectangles.size()),
                                    ShapeSet, Unsorted);
        }
        XFlush(display);
        return true;
    }
    // Offscreen has no native input region; alpha is still checked in tests.
    if (std::string_view(SDL_GetCurrentVideoDriver()) == "offscreen")
        return true;
#endif
#ifdef _WIN32
    const auto hwnd =
        static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) // Offscreen has no native input region; the renderer still draws alpha.
        return true;
    const bool custom = !opaque &&
        (SDL_GetWindowFlags(window) & (SDL_WINDOW_BORDERLESS | SDL_WINDOW_FULLSCREEN)) != 0;
    // Remove the guard BEFORE clearing the region when returning to a native
    // frame. Application does this while SDL still has BORDERLESS set, so the
    // explicit opaque request must also end suppression. Native startup never
    // installs a custom subclass at all.
    if (!custom)
    {
        if (!impl_->detach())
            return false;
    }
    // Install before changing a custom region or showing the window: the very
    // first NC activation/paint must not fall back to a second, native caption.
    else if (!impl_->attach(window, hwnd))
        return false;
    // A rectangular HRGN still disables DWM non-client rendering. Native frames
    // (and square/maximized custom windows) must remove the region, not fill one.
    if (!custom || radius <= 0)
    {
        if (SetWindowRgn(hwnd, nullptr, TRUE))
            return true;
        return SDL_SetError("Cannot clear Win32 window region: %lu", GetLastError());
    }
    // Own Win32 regions directly. The pinned SDL cannot safely remove a null
    // shape, and a cached SDL shape would otherwise be reapplied during resize.
    int native_width = 0, native_height = 0;
    if (!SDL_GetWindowSizeInPixels(window, &native_width, &native_height))
        return false;
    const float density = SDL_GetWindowPixelDensity(window);
    // HRGN is a binary VISUAL clip, not an alpha mask. A pixel-center circle
    // cuts away the renderer's partial-alpha edge and reintroduces stair steps.
    // Keep one logical AA fringe on alpha-capable surfaces; DWM supplies the
    // visible coverage. Opaque surfaces must retain their exact binary shape.
    const float fringe = (SDL_GetWindowFlags(window) & SDL_WINDOW_TRANSPARENT) ? density : 0.0f;
    const auto spans   = rounded_window_spans(native_width, native_height, radius * density, fringe);
    const auto region  = CreateRectRgn(0, 0, 0, 0);
    const auto strip   = CreateRectRgn(0, 0, 0, 0);
    bool success       = region && strip;
    if (success)
    {
        for (const auto &span : spans)
        {
            if (!SetRectRgn(strip, span.x, span.y, span.x + span.width, span.y + span.height) ||
                CombineRgn(region, region, strip, RGN_OR) == ERROR)
            {
                success = false;
                break;
            }
        }
    }
    if (strip)
        DeleteObject(strip);
    if (success && SetWindowRgn(hwnd, region, TRUE))
        return true; // Windows now owns the region.
    const auto error = GetLastError();
    if (region)
        DeleteObject(region);
    return SDL_SetError("Cannot set Win32 rounded window region: %lu", error);
#else
    int pixel_width = 0, pixel_height = 0;
    if (!SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height))
        return false;
    auto *shape = SDL_CreateSurface(pixel_width, pixel_height, SDL_PIXELFORMAT_RGBA32);
    if (!shape)
        return false;
    const auto spans = rounded_window_spans(pixel_width, pixel_height, radius * SDL_GetWindowPixelDensity(window));
    const auto white = SDL_MapSurfaceRGBA(shape, 255, 255, 255, 255);
    for (const auto &r : spans)
    {
        const SDL_Rect rect{r.x, r.y, r.width, r.height};
        SDL_FillSurfaceRect(shape, &rect, white);
    }
    const bool result = SDL_SetWindowShape(window, shape);
    SDL_DestroySurface(shape);
    return result;
#endif
}
} // namespace liltview
