#include "window_shape.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include "ui/window_chrome.hpp"

#ifdef __linux__
    #include <X11/Xlib.h>
    #include <X11/Xutil.h>
    #include <X11/extensions/shape.h>
    #include <poll.h>
    #include <wayland-client.h>
#endif

namespace liltview
{
struct WindowShape::Impl
{
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
    // Native opaque windows are never shaped, including the input region. Do
    // not send Shape/region changes that could affect the WM's native frame.
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_TRANSPARENT) == 0)
        return true;
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
            return SDL_SetError("X11 Shape extension unavailable");
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
}
bool query_window_position(SDL_Window *window, int &x, int &y)
{
    if (std::string_view(SDL_GetCurrentVideoDriver()) == "offscreen")
        return false;
#ifdef __linux__
    const auto props = SDL_GetWindowProperties(window);
    if (SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr))
        return false;
    if (auto *display = static_cast<Display *>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr)))
    {
        const auto native = static_cast<Window>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
        XWindowAttributes attributes;
        Window child = 0;
        return native && XGetWindowAttributes(display, native, &attributes) &&
            XTranslateCoordinates(display, native, attributes.root, 0, 0, &x, &y, &child);
    }
#endif
    return SDL_GetWindowPosition(window, &x, &y);
}

bool position_window_client(SDL_Window *window, int x, int y)
{
#ifdef __linux__
    const auto props = SDL_GetWindowProperties(window);
    if (auto *display = static_cast<Display *>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr)))
    {
        const auto native = static_cast<Window>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
        if (!native)
            return false;
        XSizeHints original{};
        long supplied = 0;
        XGetWMNormalHints(display, native, &original, &supplied);
        auto hints = original;
        // StaticGravity asks for the *client* position, independent of native
        // border/shadow extents. Retain this gravity for the lifetime of this
        // replacement: changing it back can itself re-anchor a window in WSLg.
        hints.flags       |= PWinGravity;
        hints.win_gravity  = StaticGravity;
        XSetWMNormalHints(display, native, &hints);
        SDL_PumpEvents();
        const auto deadline = SDL_GetTicksNS() + 100000000;
        std::vector<XEvent> configure_events;
        bool matched  = false;
        int command_x = x, command_y = y;
        for (unsigned attempt = 0; attempt < 3 && SDL_GetTicksNS() < deadline; ++attempt)
        {
            const auto serial = NextRequest(display);
            XMoveWindow(display, native, command_x, command_y);
            XFlush(display);
            bool acknowledged             = false;
            const auto collect_configures = [&]
            {
                XEvent event;
                while (XCheckTypedWindowEvent(display, native, ConfigureNotify, &event))
                {
                    configure_events.push_back(event);
                    acknowledged |= event.xconfigure.serial >= serial;
                }
            };
            int actual_x = 0, actual_y = 0;
            for (;;)
            {
                collect_configures();
                if (!query_window_position(window, actual_x, actual_y))
                    break;
                matched = std::abs(actual_x - x) <= 1 && std::abs(actual_y - y) <= 1;
                if (matched || acknowledged)
                    break;
                // A synchronous geometry query may itself have read the WM's
                // ConfigureNotify into Xlib's queue. Check before polling its fd.
                collect_configures();
                if (acknowledged)
                {
                    query_window_position(window, actual_x, actual_y);
                    matched = std::abs(actual_x - x) <= 1 && std::abs(actual_y - y) <= 1;
                    break;
                }
                const auto now = SDL_GetTicksNS();
                if (now >= deadline)
                    break;
                pollfd fd{ConnectionNumber(display), POLLIN, 0};
                const auto remaining = static_cast<int>((deadline - now + 999999) / 1000000);
                poll(&fd, 1, remaining);
            }
            if (matched)
                break;
            // WMs which ignore StaticGravity may return a frame-relative result.
            // Correct only after its actual ConfigureNotify, not an intermediate
            // SDL cached position, which can otherwise produce 32px oscillation.
            command_x += x - actual_x;
            command_y += y - actual_y;
        }
        for (auto event = configure_events.rbegin(); event != configure_events.rend(); ++event)
            XPutBackEvent(display, &*event);
        XFlush(display);
        SDL_PumpEvents(); // Deliver the retained ConfigureNotify events normally.
        return matched;
    }
#endif
    if (!SDL_SetWindowPosition(window, x, y))
        return false;
    SDL_SyncWindow(window);
    SDL_PumpEvents();
    return true;
}

} // namespace liltview
