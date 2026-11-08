#ifndef LILTVIEW_RENDERER_HPP
#define LILTVIEW_RENDERER_HPP

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

struct ImDrawData;
struct ImDrawList;

namespace liltview::render
{
enum class Backend
{
    opengl3,
    d3d11
};
enum class SurfaceKind
{
    sdl_window,
    win32_window
};
enum class ErrorCode
{
    invalid_argument,
    invalid_handle,
    dimension_limit,
    memory_budget,
    backend_failure
};

class Error : public std::runtime_error
{
public:
    Error(ErrorCode code, const char *message) :
        std::runtime_error(message),
        code(code)
    {
    }
    ErrorCode code;
};

struct Size
{
    std::uint32_t width = 0, height = 0;
};
struct Rect
{
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
};
struct Point
{
    float x = 0, y = 0;
};
struct Image
{
    std::uint64_t owner = 0;
    std::uint32_t slot = 0, generation = 0;
    explicit operator bool() const
    {
        return owner != 0;
    }
};
struct Pixels
{
    const std::uint8_t *data = nullptr;
    std::uint64_t byte_count = 0, stride_bytes = 0;
    Size size;
    // RGBA8 bytes, straight alpha, local top-left origin; no color conversion.
};
struct ImageDesc
{
    Size size;
    bool mipmaps = false;
    enum class Magnification
    {
        linear,
        nearest
    } magnification = Magnification::linear;
};
struct Capabilities
{
    Backend backend                  = Backend::opengl3;
    std::uint32_t max_texture_edge   = 0;
    std::uint64_t image_budget_bytes = 0, image_bytes = 0;
};
struct Config
{
    SurfaceKind surface_kind         = SurfaceKind::sdl_window;
    void *surface                    = nullptr; // Borrowed native window, must outlive Renderer.
    std::uint64_t image_budget_bytes = 128ULL * 1024 * 1024;
    bool vsync                       = true;
};
struct Readback
{
    Size size;
    std::vector<std::uint8_t> rgb;
}; // top-left, packed RGB8

// Render-thread only; ImGui context and native window must outlive this object.
// Errors throw render::Error; allocation failures may throw std::bad_alloc.
class Renderer
{
public:
    virtual ~Renderer()                       = default;
    virtual Capabilities capabilities() const = 0;
    virtual Size framebuffer_size() const     = 0;
    virtual Image create_image(ImageDesc)     = 0;
    // On return (also on failure) no source pointer is retained.
    virtual void update_image(Image, Rect destination, Pixels source) = 0;
    // Call after the last draw using this image was submitted, or before NewFrame.
    virtual void destroy_image(Image) noexcept                                                                              = 0;
    virtual void add_image(ImDrawList *, Image, Point min, Point max, Point uv_min = {0, 0}, Point uv_max = {1, 1})         = 0;
    virtual void add_image_quad(ImDrawList *, Image, const std::array<Point, 4> &positions, const std::array<Point, 4> &uv) = 0;
    virtual void begin_frame()                                                                                              = 0;
    virtual void render_frame(ImDrawData *)                                                                                 = 0;
    virtual void present()                                                                                                  = 0;
    virtual Readback readback() = 0; // test/tooling, before present; synchronous
};

// Exactly one backend is selected at configure time; no silent fallback.
std::unique_ptr<Renderer> create_renderer(const Config &);
// Shared checked validation; backend implementation still checks its own limits.
void validate_upload(Size image, Rect destination, Pixels source);
std::uint64_t image_storage_bytes(ImageDesc);
} // namespace liltview::render
#endif
