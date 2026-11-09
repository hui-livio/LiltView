#include "renderer/renderer.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <utility>

namespace liltview::render
{
namespace
{
std::atomic<std::uint64_t> next_owner{1};
void check_gl(const char *operation)
{
    if (glGetError() != GL_NO_ERROR)
        throw Error(ErrorCode::backend_failure, operation);
}

// Preserve caller upload state; GUI font/image updates must not inherit stride.
struct UploadState
{
    GLint texture = 0, alignment = 0, row_length = 0, skip_rows = 0, skip_pixels = 0, buffer = 0;
    PFNGLBINDBUFFERPROC bind_buffer;
    explicit UploadState(PFNGLBINDBUFFERPROC bind) :
        bind_buffer(bind)
    {
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer);
        bind_buffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    }
    ~UploadState()
    {
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
        bind_buffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(buffer));
    }
};

struct Texture
{
    GLuint name              = 0;
    std::uint32_t generation = 1;
    ImageDesc desc;
    std::uint64_t bytes = 0;
};

class OpenGLRenderer final : public Renderer
{
public:
    explicit OpenGLRenderer(const Config &config) :
        budget_(config.image_budget_bytes),
        owner_(next_owner.fetch_add(1))
    {
        if (!config.surface || config.surface_kind != SurfaceKind::sdl_window || !budget_ || !owner_)
            throw Error(ErrorCode::invalid_argument, "OpenGL renderer requires a live SDL window and image budget");
        window_  = static_cast<SDL_Window *>(config.surface);
        context_ = SDL_GL_CreateContext(window_);
        if (!context_)
            throw Error(ErrorCode::backend_failure, SDL_GetError());
        try
        {
            initialize(config.vsync);
        } catch (...)
        {
            SDL_GL_DestroyContext(context_);
            throw;
        }
    }

    ~OpenGLRenderer() override
    {
        SDL_GL_MakeCurrent(window_, context_);
        for (auto &texture : textures_)
            if (texture.name)
                glDeleteTextures(1, &texture.name);
        if (gui_ready_)
            ImGui_ImplOpenGL3_Shutdown();
        SDL_GL_DestroyContext(context_);
    }

    Capabilities capabilities() const override
    {
        return {Backend::opengl3, max_edge_, budget_, used_};
    }

    Size framebuffer_size() const override
    {
        int width = 0, height = 0;
        if (!SDL_GetWindowSizeInPixels(window_, &width, &height))
            throw Error(ErrorCode::backend_failure, SDL_GetError());
        return {static_cast<std::uint32_t>(std::max(width, 0)), static_cast<std::uint32_t>(std::max(height, 0))};
    }

    Image create_image(ImageDesc desc) override
    {
        if (desc.size.width > max_edge_ || desc.size.height > max_edge_)
            throw Error(ErrorCode::dimension_limit, "Image exceeds device texture dimensions");
        const auto bytes = image_storage_bytes(desc);
        if (bytes > budget_ - used_)
            throw Error(ErrorCode::memory_budget, "Renderer image budget exceeded");
        auto slot = vacant_slot();
        // Transparent initialization makes progressive visibility safe before all tiles arrive.
        const auto rows = std::min(desc.size.height, 64u);
        std::vector<std::uint8_t> zeros(static_cast<std::size_t>(desc.size.width) * rows * 4, 0);
        UploadState state(bind_buffer_);
        GLuint name = 0;
        glGenTextures(1, &name);
        glBindTexture(GL_TEXTURE_2D, name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, desc.mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        desc.magnification == ImageDesc::Magnification::nearest ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(desc.size.width), static_cast<GLsizei>(desc.size.height), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        for (std::uint32_t y = 0; y < desc.size.height; y += rows)
            upload_rows({0, y, desc.size.width, std::min(rows, desc.size.height - y)}, zeros.data());
        if (desc.mipmaps)
            generate_mipmap_(GL_TEXTURE_2D);
        if (!name || glGetError() != GL_NO_ERROR)
        {
            if (name)
                glDeleteTextures(1, &name);
            throw Error(ErrorCode::backend_failure, "Cannot allocate image texture");
        }
        auto &texture  = textures_[slot];
        texture.name   = name;
        texture.desc   = desc;
        texture.bytes  = bytes;
        used_         += bytes;
        return {owner_, slot, texture.generation};
    }

    Image create_uploaded_image(ImageDesc desc, Pixels source) override
    {
        if (desc.size.width > max_edge_ || desc.size.height > max_edge_)
            throw Error(ErrorCode::dimension_limit, "Image exceeds device texture dimensions");
        const auto bytes = image_storage_bytes(desc);
        validate_upload(desc.size, {0, 0, desc.size.width, desc.size.height}, source);
        if (bytes > budget_ - used_)
            throw Error(ErrorCode::memory_budget, "Renderer image budget exceeded");
        const auto slot = vacant_slot();
        UploadState state(bind_buffer_);
        GLuint name = 0;
        glGenTextures(1, &name);
        glBindTexture(GL_TEXTURE_2D, name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, desc.mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        desc.magnification == ImageDesc::Magnification::nearest ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const bool pitched = source.stride_bytes % 4 == 0 &&
            source.stride_bytes / 4 <= static_cast<std::uint64_t>(std::numeric_limits<GLint>::max());
        if (pitched)
            glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(source.stride_bytes / 4));
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(desc.size.width), static_cast<GLsizei>(desc.size.height), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, pitched ? source.data : nullptr);
        if (!pitched)
            for (std::uint32_t y = 0; y < desc.size.height; ++y)
                upload_rows({0, y, desc.size.width, 1}, source.data + static_cast<std::size_t>(y * source.stride_bytes));
        if (desc.mipmaps)
            generate_mipmap_(GL_TEXTURE_2D);
        if (!name || glGetError() != GL_NO_ERROR)
        {
            if (name)
                glDeleteTextures(1, &name);
            throw Error(ErrorCode::backend_failure, "Cannot create populated image texture");
        }
        auto &texture  = textures_[slot];
        texture.name   = name;
        texture.desc   = desc;
        texture.bytes  = bytes;
        used_         += bytes;
        return {owner_, slot, texture.generation};
    }

    void update_image(Image image, Rect destination, Pixels source) override
    {
        auto &texture = get(image);
        validate_upload(texture.desc.size, destination, source);
        UploadState state(bind_buffer_);
        glBindTexture(GL_TEXTURE_2D, texture.name);
        // Non-multiple-of-four pitches are legal. Upload rows without staging a full image.
        if (source.stride_bytes % 4 == 0 &&
            source.stride_bytes / 4 <= static_cast<std::uint64_t>(std::numeric_limits<GLint>::max()))
        {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(source.stride_bytes / 4));
            upload_rows(destination, source.data);
        }
        else
            for (std::uint32_t y = 0; y < destination.height; ++y)
                upload_rows({destination.x, destination.y + y, destination.width, 1},
                            source.data + static_cast<std::size_t>(y * source.stride_bytes));
        if (texture.desc.mipmaps)
            generate_mipmap_(GL_TEXTURE_2D);
        check_gl("Image upload failed");
    }

    void destroy_image(Image image) noexcept override
    {
        if (!valid(image))
            return;
        auto &texture = textures_[image.slot];
        glDeleteTextures(1, &texture.name);
        used_        -= texture.bytes;
        texture.name  = 0;
        // Retire a slot on generation exhaustion rather than resurrect stale handles.
        if (texture.generation == UINT32_MAX)
            texture.generation = 0;
        else
            ++texture.generation;
    }

    void add_image(ImDrawList *list, Image image, Point min, Point max, Point uv_min, Point uv_max) override
    {
        if (!list)
            throw Error(ErrorCode::invalid_argument, "Missing image draw list");
        const auto &texture = get(image);
        list->AddImage(static_cast<ImTextureID>(texture.name), {min.x, min.y}, {max.x, max.y}, {uv_min.x, uv_min.y},
                       {uv_max.x, uv_max.y});
    }

    void begin_frame() override
    {
        ImGui_ImplOpenGL3_NewFrame();
    }

    void add_image_quad(ImDrawList *list, Image image, const std::array<Point, 4> &p, const std::array<Point, 4> &uv) override
    {
        if (!list)
            throw Error(ErrorCode::invalid_argument, "Missing image draw list");
        const auto &texture = get(image);
        list->AddImageQuad(static_cast<ImTextureID>(texture.name), {p[0].x, p[0].y}, {p[1].x, p[1].y}, {p[2].x, p[2].y},
                           {p[3].x, p[3].y}, {uv[0].x, uv[0].y}, {uv[1].x, uv[1].y}, {uv[2].x, uv[2].y}, {uv[3].x, uv[3].y});
    }

    void render_frame(ImDrawData *data) override
    {
        const auto size = framebuffer_size();
        if (!size.width || !size.height)
            return;
        glViewport(0, 0, static_cast<GLsizei>(size.width), static_cast<GLsizei>(size.height));
        glClearColor(0.055f, 0.063f, 0.075f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(data);
        check_gl("Frame render failed");
    }

    void present() override
    {
        const auto size = framebuffer_size();
        if (size.width && size.height && !SDL_GL_SwapWindow(window_))
            throw Error(ErrorCode::backend_failure, SDL_GetError());
    }

    Readback readback() override
    {
        Readback result{framebuffer_size(), {}};
        if (!result.size.width || !result.size.height)
            return result;
        const auto row = static_cast<std::size_t>(result.size.width) * 3;
        result.rgb.resize(row * result.size.height);
        GLint alignment = 0;
        glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, static_cast<GLsizei>(result.size.width), static_cast<GLsizei>(result.size.height), GL_RGB,
                     GL_UNSIGNED_BYTE, result.rgb.data());
        glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        check_gl("Screenshot readback failed");
        for (std::uint32_t y = 0; y < result.size.height / 2; ++y)
            std::swap_ranges(result.rgb.begin() + static_cast<std::ptrdiff_t>(y * row),
                             result.rgb.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                             result.rgb.begin() + static_cast<std::ptrdiff_t>((result.size.height - 1 - y) * row));
        return result;
    }

private:
    void initialize(bool vsync)
    {
        if (!SDL_GL_MakeCurrent(window_, context_))
            throw Error(ErrorCode::backend_failure, SDL_GetError());
        // Unsupported VSync is non-fatal; rendering still works without it.
        SDL_GL_SetSwapInterval(vsync ? 1 : 0);
        generate_mipmap_ = reinterpret_cast<PFNGLGENERATEMIPMAPPROC>(SDL_GL_GetProcAddress("glGenerateMipmap"));
        bind_buffer_     = reinterpret_cast<PFNGLBINDBUFFERPROC>(SDL_GL_GetProcAddress("glBindBuffer"));
        if (!generate_mipmap_ || !bind_buffer_)
            throw Error(ErrorCode::backend_failure, "Missing OpenGL 3.3 entry points");
        GLint maximum = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
        if (maximum <= 0)
            throw Error(ErrorCode::backend_failure, "Invalid device texture limit");
        max_edge_ = static_cast<std::uint32_t>(std::max(maximum, 0));
        check_gl("Texture capability query failed");
        gui_ready_ = ImGui_ImplOpenGL3_Init("#version 330 core");
        if (!gui_ready_)
            throw Error(ErrorCode::backend_failure, "ImGui OpenGL backend initialization failed");
    }
    bool valid(Image image) const
    {
        return image.owner == owner_ && image.slot < textures_.size() && textures_[image.slot].name &&
            image.generation == textures_[image.slot].generation;
    }
    Texture &get(Image image)
    {
        if (!valid(image))
            throw Error(ErrorCode::invalid_handle, "Stale or foreign renderer image handle");
        return textures_[image.slot];
    }
    std::uint32_t vacant_slot()
    {
        for (std::size_t i = 0; i < textures_.size(); ++i)
            if (!textures_[i].name && textures_[i].generation)
                return static_cast<std::uint32_t>(i);
        if (textures_.size() >= UINT32_MAX)
            throw Error(ErrorCode::dimension_limit, "Renderer handle limit");
        textures_.emplace_back();
        return static_cast<std::uint32_t>(textures_.size() - 1);
    }
    static void upload_rows(Rect rect, const std::uint8_t *data)
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(rect.x), static_cast<GLint>(rect.y),
                        static_cast<GLsizei>(rect.width), static_cast<GLsizei>(rect.height), GL_RGBA, GL_UNSIGNED_BYTE, data);
    }
    SDL_Window *window_                      = nullptr;
    SDL_GLContext context_                   = nullptr;
    PFNGLGENERATEMIPMAPPROC generate_mipmap_ = nullptr;
    PFNGLBINDBUFFERPROC bind_buffer_         = nullptr;
    std::vector<Texture> textures_;
    std::uint64_t budget_ = 0, used_ = 0, owner_ = 0;
    std::uint32_t max_edge_ = 0;
    bool gui_ready_         = false;
};
} // namespace

std::unique_ptr<Renderer> create_renderer(const Config &config)
{
    return std::make_unique<OpenGLRenderer>(config);
}
} // namespace liltview::render
