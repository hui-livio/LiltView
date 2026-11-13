#include "renderer/renderer.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <limits>
#include <utility>

namespace liltview::render
{
namespace
{
std::atomic<std::uint64_t> next_owner{1};
constexpr std::size_t upload_chunk_bytes = 1024 * 1024;
void check(bool result)
{
    if (!result)
        throw Error(ErrorCode::backend_failure, SDL_GetError());
}
struct TextureDeleter
{
    void operator()(SDL_Texture *texture) const noexcept
    {
        SDL_DestroyTexture(texture);
    }
};
using TexturePtr = std::unique_ptr<SDL_Texture, TextureDeleter>;
struct SurfaceDeleter
{
    void operator()(SDL_Surface *surface) const noexcept
    {
        SDL_DestroySurface(surface);
    }
};
using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;

// Texture uploads/mipmap generation must not change the target or clipping of
// the current frame. SDL may queue these commands, but never retains client pixels.
struct TargetState
{
    SDL_Renderer *renderer;
    SDL_Texture *target;
    SDL_Rect viewport{}, clip{};
    bool viewport_set, clip_set;
    float scale_x = 1, scale_y = 1;
    explicit TargetState(SDL_Renderer *value) :
        renderer(value),
        target(SDL_GetRenderTarget(value)),
        viewport_set(SDL_RenderViewportSet(value)),
        clip_set(SDL_RenderClipEnabled(value))
    {
        check(SDL_GetRenderViewport(renderer, &viewport));
        check(SDL_GetRenderClipRect(renderer, &clip));
        check(SDL_GetRenderScale(renderer, &scale_x, &scale_y));
    }
    ~TargetState()
    {
        SDL_SetRenderTarget(renderer, target);
        SDL_SetRenderScale(renderer, scale_x, scale_y);
        SDL_SetRenderViewport(renderer, viewport_set ? &viewport : nullptr);
        SDL_SetRenderClipRect(renderer, clip_set ? &clip : nullptr);
    }
};
struct SamplingState
{
    SDL_Texture *texture;
    SDL_BlendMode blend = SDL_BLENDMODE_BLEND;
    SDL_ScaleMode scale = SDL_SCALEMODE_LINEAR;
    explicit SamplingState(SDL_Texture *value) :
        texture(value)
    {
        check(SDL_GetTextureBlendMode(texture, &blend));
        check(SDL_GetTextureScaleMode(texture, &scale));
    }
    ~SamplingState()
    {
        SDL_SetTextureBlendMode(texture, blend);
        SDL_SetTextureScaleMode(texture, scale);
    }
};
struct Texture
{
    std::vector<TexturePtr> levels;
    ImageDesc desc;
    std::uint32_t generation = 1;
    std::uint64_t bytes      = 0;
};
struct Sampling
{
    SDL_Texture *texture;
    SDL_ScaleMode mode;
};
void apply_sampling(const ImDrawList *, const ImDrawCmd *command)
{
    const auto &sampling = *static_cast<const Sampling *>(command->UserCallbackData);
    check(SDL_SetTextureScaleMode(sampling.texture, sampling.mode));
}

class SDLRenderer final : public Renderer
{
public:
    explicit SDLRenderer(const Config &config) :
        budget_(config.image_budget_bytes),
        owner_(next_owner.fetch_add(1))
    {
        if (!config.surface || config.surface_kind != SurfaceKind::sdl_window || !budget_ || !owner_)
            throw Error(ErrorCode::invalid_argument, "SDL renderer requires a live SDL window and image budget");
        window_   = static_cast<SDL_Window *>(config.surface);
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (!renderer_)
            throw Error(ErrorCode::backend_failure, SDL_GetError());
        try
        {
            // Unsupported vsync must not prevent software/remote/headless rendering.
            SDL_SetRenderVSync(renderer_, config.vsync ? 1 : 0);
            const auto limit =
                SDL_GetNumberProperty(SDL_GetRendererProperties(renderer_), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0);
            // SDL reports zero when the driver has no explicit dimension limit.
            max_edge_ = static_cast<std::uint32_t>(std::min<Sint64>(limit > 0 ? limit : INT_MAX / 4, INT_MAX / 4));
            if (!ImGui_ImplSDLRenderer3_Init(renderer_))
                throw Error(ErrorCode::backend_failure, "ImGui SDL renderer initialization failed");
            gui_ready_ = true;
        } catch (...)
        {
            SDL_DestroyRenderer(renderer_);
            throw;
        }
    }
    ~SDLRenderer() override
    {
        textures_.clear();
        frame_.reset();
        if (gui_ready_)
            ImGui_ImplSDLRenderer3_Shutdown();
        SDL_DestroyRenderer(renderer_);
    }
    Capabilities capabilities() const override
    {
        return {Backend::sdl_renderer, max_edge_, budget_, used_};
    }
    UploadStats upload_stats() const override
    {
        return {0, 0, 0, direct_uploads_};
    }
    Size framebuffer_size() const override
    {
        int width = 0, height = 0;
        check(SDL_GetRenderOutputSize(renderer_, &width, &height));
        return {static_cast<std::uint32_t>(std::max(width, 0)), static_cast<std::uint32_t>(std::max(height, 0))};
    }
    Image create_image(ImageDesc desc) override
    {
        return create(desc, nullptr);
    }
    Image create_uploaded_image(ImageDesc desc, Pixels source) override
    {
        validate_upload(desc.size, {0, 0, desc.size.width, desc.size.height}, source);
        return create(desc, &source);
    }
    void update_image(Image image, Rect destination, Pixels source) override
    {
        auto &texture = get(image);
        validate_upload(texture.desc.size, destination, source);
        upload(texture.levels.front().get(), destination, source);
        generate_mipmaps(texture);
    }
    void destroy_image(Image image) noexcept override
    {
        if (!valid(image))
            return;
        auto &texture = textures_[image.slot];
        texture.levels.clear();
        used_         -= texture.bytes;
        texture.bytes  = 0;
        // Exhausted generations retire the slot instead of reviving stale handles.
        texture.generation = texture.generation == UINT32_MAX ? 0 : texture.generation + 1;
    }
    void add_image(ImDrawList *list, Image image, Point min, Point max, Point uv_min, Point uv_max) override
    {
        const std::array<Point, 4> p{min, Point{max.x, min.y}, max, Point{min.x, max.y}};
        const std::array<Point, 4> uv{uv_min, Point{uv_max.x, uv_min.y}, uv_max, Point{uv_min.x, uv_max.y}};
        add_image_quad(list, image, p, uv);
    }
    void add_image_quad(ImDrawList *list, Image image, const std::array<Point, 4> &p, const std::array<Point, 4> &uv) override
    {
        if (!list)
            throw Error(ErrorCode::invalid_argument, "Missing image draw list");
        const auto &texture = get(image);
        const auto density  = ImGui::GetIO().DisplayFramebufferScale;
        float ratio         = 0;
        for (const auto i : {1, 3})
        {
            const auto pixels = std::hypot((p[i].x - p[0].x) * density.x, (p[i].y - p[0].y) * density.y);
            const auto texels =
                std::hypot((uv[i].x - uv[0].x) * texture.desc.size.width, (uv[i].y - uv[0].y) * texture.desc.size.height);
            if (pixels > 0)
                ratio = std::max(ratio, texels / pixels);
        }
        const auto lod   = ratio > 1 ? std::log2(ratio) : 0;
        const auto level = static_cast<std::size_t>(std::min(lod, static_cast<float>(texture.levels.size() - 1)));
        Sampling sampling{texture.levels[level].get(),
                          ratio <= 1 && texture.desc.magnification == ImageDesc::Magnification::nearest ? SDL_SCALEMODE_NEAREST :
                                                                                                          SDL_SCALEMODE_LINEAR};
        // Sampling is per draw, not per handle: the same texture can be drawn
        // magnified and minified in one ImGui frame without changing earlier draws.
        // Atlas/linear-only draws stay batchable; only native base textures
        // need a sampler callback to distinguish magnification/minification.
        if (level == 0 && texture.desc.magnification == ImageDesc::Magnification::nearest)
            list->AddCallback(apply_sampling, &sampling, sizeof(sampling));
        list->AddImageQuad(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(sampling.texture)), {p[0].x, p[0].y},
                           {p[1].x, p[1].y}, {p[2].x, p[2].y}, {p[3].x, p[3].y}, {uv[0].x, uv[0].y}, {uv[1].x, uv[1].y},
                           {uv[2].x, uv[2].y}, {uv[3].x, uv[3].y});
    }
    void begin_frame() override
    {
        ImGui_ImplSDLRenderer3_NewFrame();
    }
    void render_frame(ImDrawData *data) override
    {
        const auto size = framebuffer_size();
        if (!size.width || !size.height)
            return;
        if (!frame_ || frame_size_.width != size.width || frame_size_.height != size.height)
        {
            TexturePtr next(SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
                                              static_cast<int>(size.width), static_cast<int>(size.height)));
            if (!next)
                throw Error(ErrorCode::backend_failure, SDL_GetError());
            check(SDL_SetTextureBlendMode(next.get(), SDL_BLENDMODE_NONE));
            check(SDL_SetTextureScaleMode(next.get(), SDL_SCALEMODE_NEAREST));
            check(SDL_SetRenderTarget(renderer_, nullptr));
            frame_      = std::move(next);
            frame_size_ = size;
        }
        check(SDL_SetRenderTarget(renderer_, frame_.get()));
        check(SDL_SetRenderViewport(renderer_, nullptr));
        check(SDL_SetRenderClipRect(renderer_, nullptr));
        // SDLRenderer3 expects SDL to scale vertex positions on HiDPI output.
        check(SDL_SetRenderScale(renderer_, data->FramebufferScale.x, data->FramebufferScale.y));
        const auto flags = SDL_GetWindowFlags(window_);
        if ((flags & SDL_WINDOW_TRANSPARENT) != 0)
            check(SDL_SetRenderDrawColorFloat(renderer_, 0, 0, 0, 0));
        else if ((flags & SDL_WINDOW_BORDERLESS) != 0)
            check(SDL_SetRenderDrawColorFloat(renderer_, opaque_background_[0], opaque_background_[1], opaque_background_[2], 0));
        else
            check(SDL_SetRenderDrawColorFloat(renderer_, 0.055f, 0.063f, 0.075f, 1));
        check(SDL_RenderClear(renderer_));
        ImGui_ImplSDLRenderer3_RenderDrawData(data, renderer_);
        // Submit now so rendering errors cannot remain hidden in the command queue.
        check(SDL_FlushRenderer(renderer_));
    }
    void present() override
    {
        if (!frame_)
            return;
        check(SDL_SetRenderTarget(renderer_, nullptr));
        check(SDL_SetRenderViewport(renderer_, nullptr));
        check(SDL_SetRenderClipRect(renderer_, nullptr));
        check(SDL_SetRenderScale(renderer_, 1, 1));
        check(SDL_RenderTexture(renderer_, frame_.get(), nullptr, nullptr));
        check(SDL_RenderPresent(renderer_));
    }
    Readback readback() override
    {
        if (!frame_)
            return {};
        TargetState state(renderer_);
        check(SDL_SetRenderTarget(renderer_, frame_.get()));
        SurfacePtr surface(SDL_RenderReadPixels(renderer_, nullptr));
        if (!surface)
            throw Error(ErrorCode::backend_failure, SDL_GetError());
        SurfacePtr rgba(SDL_ConvertSurface(surface.get(), SDL_PIXELFORMAT_RGBA32));
        if (!rgba)
            throw Error(ErrorCode::backend_failure, SDL_GetError());
        Readback result{{static_cast<std::uint32_t>(rgba->w), static_cast<std::uint32_t>(rgba->h)}, {}, {}};
        const auto count = static_cast<std::size_t>(rgba->w) * rgba->h;
        result.rgb.resize(count * 3);
        const bool alpha = (SDL_GetWindowFlags(window_) & (SDL_WINDOW_TRANSPARENT | SDL_WINDOW_BORDERLESS)) != 0;
        if (alpha)
            result.alpha.resize(count);
        for (int y = 0; y < rgba->h; ++y)
        {
            const auto *row = static_cast<const std::uint8_t *>(rgba->pixels) + static_cast<std::size_t>(y) * rgba->pitch;
            for (int x = 0; x < rgba->w; ++x)
            {
                const auto at = static_cast<std::size_t>(y) * rgba->w + x;
                std::copy_n(row + x * 4, 3, result.rgb.data() + at * 3);
                if (alpha)
                    result.alpha[at] = row[x * 4 + 3];
            }
        }
        return result;
    }

private:
    Image create(ImageDesc desc, const Pixels *source)
    {
        if (desc.size.width > max_edge_ || desc.size.height > max_edge_)
            throw Error(ErrorCode::dimension_limit, "Image exceeds device texture dimensions");
        const auto bytes = image_storage_bytes(desc);
        if (bytes > budget_ - used_)
            throw Error(ErrorCode::memory_budget, "Renderer image budget exceeded");
        const auto slot = vacant_slot();
        Texture allocation;
        allocation.desc  = desc;
        allocation.bytes = bytes;
        for (auto size = desc.size;; size = {std::max(1u, size.width / 2), std::max(1u, size.height / 2)})
        {
            TexturePtr texture(SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                                 desc.mipmaps ? SDL_TEXTUREACCESS_TARGET : SDL_TEXTUREACCESS_STATIC,
                                                 static_cast<int>(size.width), static_cast<int>(size.height)));
            if (!texture)
                throw Error(ErrorCode::backend_failure, SDL_GetError());
            check(SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND));
            check(SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_LINEAR));
            allocation.levels.push_back(std::move(texture));
            if (!desc.mipmaps || (size.width == 1 && size.height == 1))
                break;
        }
        if (source)
            upload(allocation.levels.front().get(), {0, 0, desc.size.width, desc.size.height}, *source);
        else
        {
            // Bounded transparent initialization keeps progressive partial uploads safe.
            const auto pitch = static_cast<std::size_t>(desc.size.width) * 4;
            const auto rows =
                std::min(desc.size.height, static_cast<std::uint32_t>(std::max<std::size_t>(1, upload_chunk_bytes / pitch)));
            const std::vector<std::uint8_t> zeros(pitch * rows, 0);
            for (std::uint32_t y = 0; y < desc.size.height; y += rows)
            {
                const auto height = std::min(rows, desc.size.height - y);
                upload(allocation.levels.front().get(), {0, y, desc.size.width, height},
                       {zeros.data(), zeros.size(), pitch, {desc.size.width, height}});
            }
        }
        generate_mipmaps(allocation);
        auto &texture   = textures_[slot];
        texture.levels  = std::move(allocation.levels);
        texture.desc    = desc;
        texture.bytes   = bytes;
        used_          += bytes;
        return {owner_, slot, texture.generation};
    }
    void upload(SDL_Texture *texture, Rect destination, Pixels source)
    {
        SDL_Rect rect{static_cast<int>(destination.x), static_cast<int>(destination.y), static_cast<int>(destination.width),
                      static_cast<int>(destination.height)};
        // SDL driver pitch implementations need not support non-RGBA-aligned
        // rows. Repack odd/oversized pitches in bounded strips, never whole images.
        const auto pitch = static_cast<std::size_t>(source.size.width) * 4;
        if (source.stride_bytes <= INT_MAX && source.stride_bytes % 4 == 0)
            check(SDL_UpdateTexture(texture, &rect, source.data, static_cast<int>(source.stride_bytes)));
        else
        {
            const auto rows =
                std::min(source.size.height, static_cast<std::uint32_t>(std::max<std::size_t>(1, upload_chunk_bytes / pitch)));
            std::vector<std::uint8_t> packed(pitch * rows);
            for (std::uint32_t y = 0; y < source.size.height; y += rows)
            {
                const auto height = std::min(rows, source.size.height - y);
                for (std::uint32_t row = 0; row < height; ++row)
                    std::copy_n(source.data + static_cast<std::size_t>(y + row) * source.stride_bytes, pitch,
                                packed.data() + static_cast<std::size_t>(row) * pitch);
                rect.y = static_cast<int>(destination.y + y);
                rect.h = static_cast<int>(height);
                check(SDL_UpdateTexture(texture, &rect, packed.data(), static_cast<int>(pitch)));
            }
        }
        ++direct_uploads_;
    }
    void generate_mipmaps(Texture &texture)
    {
        if (texture.levels.size() < 2)
            return;
        TargetState state(renderer_);
        for (std::size_t level = 1; level < texture.levels.size(); ++level)
        {
            auto *source = texture.levels[level - 1].get();
            SamplingState sampling(source);
            check(SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE));
            check(SDL_SetTextureScaleMode(source, SDL_SCALEMODE_LINEAR));
            check(SDL_SetRenderTarget(renderer_, texture.levels[level].get()));
            check(SDL_SetRenderViewport(renderer_, nullptr));
            check(SDL_SetRenderClipRect(renderer_, nullptr));
            check(SDL_SetRenderScale(renderer_, 1, 1));
            check(SDL_RenderTexture(renderer_, source, nullptr, nullptr));
        }
        // Ensure the last target is no longer bound before committing/destroying textures.
    }
    bool valid(Image image) const noexcept
    {
        return image.owner == owner_ && image.slot < textures_.size() && !textures_[image.slot].levels.empty() &&
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
            if (textures_[i].levels.empty() && textures_[i].generation)
                return static_cast<std::uint32_t>(i);
        if (textures_.size() >= UINT32_MAX)
            throw Error(ErrorCode::dimension_limit, "Renderer handle limit");
        textures_.emplace_back();
        return static_cast<std::uint32_t>(textures_.size() - 1);
    }
    SDL_Window *window_     = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    TexturePtr frame_;
    Size frame_size_;
    std::vector<Texture> textures_;
    std::uint64_t budget_ = 0, used_ = 0, owner_ = 0, direct_uploads_ = 0;
    std::uint32_t max_edge_ = 0;
    bool gui_ready_         = false;
};
} // namespace
std::unique_ptr<Renderer> create_renderer(const Config &config)
{
    return std::make_unique<SDLRenderer>(config);
}
} // namespace liltview::render
