#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <stdexcept>
#include <vector>
#include "platform/sdl_render_host.hpp"
#include "renderer/renderer.hpp"

namespace
{
void check(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
void exercise(liltview::render::Renderer &renderer)
{
    using namespace liltview::render;
    const auto base = renderer.capabilities().image_bytes;
    auto image      = renderer.create_image({{4, 4}, false});
    check(renderer.capabilities().image_bytes == base + 64, "image accounting");
    std::array<std::uint8_t, 80> data{};
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 4; ++x)
        {
            data[y * 20 + x * 4]     = 220;
            data[y * 20 + x * 4 + 1] = 40;
            data[y * 20 + x * 4 + 2] = 30;
            data[y * 20 + x * 4 + 3] = 255;
        }
    GLint old_texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 17);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    renderer.update_image(image, {0, 0, 4, 4}, {data.data(), data.size(), 20, {4, 4}});
    GLint row = 0, alignment = 0, texture = 0;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    check(row == 17 && alignment == 8 && texture == old_texture, "upload state restore");
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    renderer.begin_frame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    renderer.add_image(ImGui::GetForegroundDrawList(), image, {10, 10}, {110, 110});
    ImGui::Render();
    renderer.render_frame(ImGui::GetDrawData());
    const auto pixels = renderer.readback();
    const auto pixel  = (static_cast<std::size_t>(50) * pixels.size.width + 50) * 3;
    check(pixels.rgb[pixel] > 200 && pixels.rgb[pixel + 1] < 60 && pixels.rgb[pixel + 2] < 50, "image rendered color");
    renderer.present();
    renderer.destroy_image(image);
    auto replacement = renderer.create_image({{4, 4}, false});
    check(replacement.generation != image.generation, "generation reuse");
    bool rejected = false;
    try
    {
        renderer.update_image(image, {0, 0, 4, 4}, {data.data(), data.size(), 20, {4, 4}});
    } catch (const Error &e)
    {
        rejected = e.code == ErrorCode::invalid_handle;
    }
    check(rejected, "stale handle rejection");
    renderer.destroy_image(image);
    renderer.destroy_image(replacement);
    check(renderer.capabilities().image_bytes == base, "destroy accounting");
    rejected = false;
    try
    {
        renderer.create_image({{renderer.capabilities().max_texture_edge + 1, 1}, false});
    } catch (const Error &e)
    {
        rejected = e.code == ErrorCode::dimension_limit;
    }
    check(rejected, "texture limit rejection");
}
void verify_populated_creation(liltview::render::Renderer &renderer)
{
    using namespace liltview::render;
    const auto base = renderer.capabilities().image_bytes;
    for (unsigned stride : {16u, 17u, 20u})
    {
        std::vector<std::uint8_t> data(stride * 3 + 16, 0);
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 4; ++x)
            {
                data[y * stride + x * 4]     = 220;
                data[y * stride + x * 4 + 1] = 40;
                data[y * stride + x * 4 + 2] = 30;
                data[y * stride + x * 4 + 3] = 255;
            }
        GLint old_texture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 19);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 3);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2);
        const ImageDesc desc{{4, 4}, true};
        auto image = renderer.create_uploaded_image(desc, {data.data(), data.size(), stride, {4, 4}});
        check(renderer.capabilities().image_bytes == base + image_storage_bytes(desc), "Populated mipmap accounting");
        GLint row = 0, alignment = 0, texture = 0, skip_rows = 0, skip_pixels = 0;
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
        check(row == 19 && alignment == 8 && texture == old_texture && skip_rows == 3 && skip_pixels == 2,
              "Populated creation did not restore GL unpack state");
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        std::fill(data.begin(), data.end(), 0); // No source pointer may be retained.
        renderer.begin_frame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        renderer.add_image(ImGui::GetForegroundDrawList(), image, {10, 10}, {110, 110});
        ImGui::Render();
        renderer.render_frame(ImGui::GetDrawData());
        const auto pixels = renderer.readback();
        const auto at     = (static_cast<std::size_t>(50) * pixels.size.width + 50) * 3;
        check(pixels.rgb[at] > 210 && pixels.rgb[at + 1] < 50 && pixels.rgb[at + 2] < 40,
              "Populated texture is incomplete or retained client data");
        renderer.present();
        renderer.destroy_image(image);
        check(renderer.capabilities().image_bytes == base, "Populated texture leaked");
    }
    const std::array<std::uint8_t, 4> short_source{};
    bool rejected = false;
    try
    {
        renderer.create_uploaded_image({{4, 4}, true}, {short_source.data(), short_source.size(), 16, {4, 4}});
    } catch (const Error &error)
    {
        rejected = error.code == ErrorCode::invalid_argument;
    }
    check(rejected && renderer.capabilities().image_bytes == base, "Invalid initial pixels allocated or leaked a texture");
}

void verify_staging(liltview::render::Renderer &renderer, bool staged)
{
    using namespace liltview::render;
    const auto gen       = reinterpret_cast<PFNGLGENBUFFERSPROC>(SDL_GL_GetProcAddress("glGenBuffers"));
    const auto bind      = reinterpret_cast<PFNGLBINDBUFFERPROC>(SDL_GL_GetProcAddress("glBindBuffer"));
    const auto allocate  = reinterpret_cast<PFNGLBUFFERDATAPROC>(SDL_GL_GetProcAddress("glBufferData"));
    const auto remove    = reinterpret_cast<PFNGLDELETEBUFFERSPROC>(SDL_GL_GetProcAddress("glDeleteBuffers"));
    GLuint caller_buffer = 0;
    gen(1, &caller_buffer);
    bind(GL_PIXEL_UNPACK_BUFFER, caller_buffer);
    allocate(GL_PIXEL_UNPACK_BUFFER, 1024, nullptr, GL_STREAM_DRAW);
    const auto base   = renderer.capabilities().image_bytes;
    const auto before = renderer.upload_stats();
    std::vector<Image> images;
    for (unsigned n = 0; n < 64; ++n)
    {
        constexpr unsigned stride = 129;
        std::vector<std::uint8_t> data(stride * 31 + 128, 0);
        for (unsigned y = 0; y < 32; ++y)
            for (unsigned x = 0; x < 32; ++x)
            {
                const auto at = y * stride + x * 4;
                data[at]      = static_cast<std::uint8_t>(n * 3);
                data[at + 1]  = 120;
                data[at + 2]  = 210;
                data[at + 3]  = 255;
            }
        images.push_back(renderer.create_uploaded_image({{32, 32}, true}, {data.data(), data.size(), stride, {32, 32}}));
        GLint current = 0;
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &current);
        check(static_cast<GLuint>(current) == caller_buffer, "Caller PBO binding was not restored");
        std::fill(data.begin(), data.end(), 0); // No client pointer may survive a queued transfer.
        const auto stats = renderer.upload_stats();
        check(stats.staging_bytes <= stats.staging_limit, "Upload staging exceeded its fixed budget");
    }
    bind(GL_PIXEL_UNPACK_BUFFER, 0);
    remove(1, &caller_buffer);
    renderer.begin_frame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    for (unsigned n = 0; n < images.size(); ++n)
    {
        const auto x = static_cast<float>((n % 8) * 16), y = static_cast<float>((n / 8) * 16);
        renderer.add_image(ImGui::GetForegroundDrawList(), images[n], {x, y}, {x + 16, y + 16});
    }
    ImGui::Render();
    renderer.render_frame(ImGui::GetDrawData());
    const auto pixels = renderer.readback();
    for (unsigned n = 0; n < images.size(); ++n)
    {
        const auto at = (static_cast<std::size_t>((n / 8) * 16 + 8) * pixels.size.width + (n % 8) * 16 + 8) * 3;
        check(std::abs(static_cast<int>(pixels.rgb[at]) - static_cast<int>(n * 3)) <= 1 && pixels.rgb[at + 1] == 120 &&
                  pixels.rgb[at + 2] == 210,
              "Queued uploads corrupted/reused in-flight buffer data");
    }
    renderer.present();
    const auto stats = renderer.upload_stats();
    check(stats.staged_uploads + stats.direct_uploads >= before.staged_uploads + before.direct_uploads + images.size(),
          "Upload transfer accounting lost requests");
    check(!staged || stats.staged_uploads > before.staged_uploads, "PBO path was not exercised");
    check(staged || (stats.staged_uploads == 0 && stats.staging_bytes == 0 && stats.staging_limit == 0),
          "Direct diagnostic mode allocated staging buffers");
    for (const auto image : images)
        renderer.destroy_image(image);
    check(renderer.capabilities().image_bytes == base, "Staged images leaked texture accounting");
}

void verify_magnification(liltview::render::Renderer &renderer)
{
    using namespace liltview::render;
    auto image = renderer.create_image({{2, 2}, false, ImageDesc::Magnification::nearest});
    // Pixel blocks inside one texture: linear filtering would blur these edges.
    const std::array<std::uint8_t, 16> board{255, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 255};
    renderer.update_image(image, {0, 0, 2, 2}, {board.data(), board.size(), 8, {2, 2}});
    renderer.begin_frame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    renderer.add_image(ImGui::GetForegroundDrawList(), image, {0, 0}, {128, 128});
    ImGui::Render();
    renderer.render_frame(ImGui::GetDrawData());
    const auto pixels = renderer.readback();
    for (unsigned y = 1; y < 127; ++y)
        for (unsigned x = 1; x < 127; ++x)
        {
            const bool white = (x >= 64) != (y >= 64);
            const auto at    = (static_cast<std::size_t>(y) * pixels.size.width + x) * 3;
            check(pixels.rgb[at] == 255 && pixels.rgb[at + 1] == (white ? 255 : 0) && pixels.rgb[at + 2] == (white ? 255 : 0),
                  "Magnified pixel boundary was interpolated");
        }
    renderer.present();
    renderer.destroy_image(image);
}

void verify_opaque_background(liltview::render::Renderer &renderer, SDL_Window *window)
{
    check((SDL_GetWindowFlags(window) & SDL_WINDOW_TRANSPARENT) == 0, "Opaque clear test needs an opaque surface");
    check(SDL_SetWindowBordered(window, false), SDL_GetError());
    for (const std::array<float, 3> color : {std::array<float, 3>{0.08f, 0.31f, 0.68f}, {0.38f, 0.70f, 1.0f}})
    {
        renderer.set_opaque_background(color);
        renderer.begin_frame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        // Leave the top-left pixel untouched to inspect the RGB/alpha clear.
        ImGui::GetBackgroundDrawList()->AddRectFilled({20, 20}, {40, 40}, IM_COL32_WHITE);
        ImGui::Render();
        renderer.render_frame(ImGui::GetDrawData());
        const auto frame = renderer.readback();
        check(frame.rgb.size() >= 3 && !frame.alpha.empty(), "Missing opaque clear readback");
        for (unsigned channel = 0; channel < 3; ++channel)
            check(std::abs(static_cast<int>(frame.rgb[channel]) - static_cast<int>(std::lround(color[channel] * 255))) <= 1,
                  "Opaque borderless clear did not use the supplied border color");
        check(frame.alpha[0] == 0, "Opaque clear lost rounded-corner diagnostic alpha");
        renderer.present();
    }
    check(SDL_SetWindowBordered(window, true), SDL_GetError());
    std::puts("PASS opaque borderless clear follows application color changes without a background halo");
}

void verify_decoration_switch(liltview::render::Renderer &renderer, SDL_Window *window)
{
    using namespace liltview::render;
    const auto original_window  = SDL_GetWindowID(window);
    const auto original_context = SDL_GL_GetCurrentContext();
    const auto base             = renderer.capabilities().image_bytes;
    std::array<std::uint8_t, 16> pixels{230, 30, 40, 255, 230, 30, 40, 255, 230, 30, 40, 255, 230, 30, 40, 255};
    auto image           = renderer.create_uploaded_image({{2, 2}, false}, {pixels.data(), pixels.size(), 8, {2, 2}});
    const auto accounted = renderer.capabilities().image_bytes;
    for (const bool custom : {true, false, true, false})
    {
        check(SDL_SetWindowBordered(window, !custom), SDL_GetError());
        check(SDL_SetWindowSize(window, 160, 140), SDL_GetError());
        SDL_SyncWindow(window);
        check(SDL_GetWindowID(window) == original_window, "Decoration switch replaced the native window");
        check(SDL_GL_GetCurrentContext() == original_context, "Decoration switch replaced the GL context");
        check(renderer.capabilities().image_bytes == accounted, "Decoration switch lost or duplicated GPU images");
        renderer.begin_frame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        renderer.add_image(ImGui::GetBackgroundDrawList(), image, {0, 0}, {160, 140});
        ImGui::GetForegroundDrawList()->AddText({8, 8}, IM_COL32_WHITE, "Preserved font texture");
        ImGui::Render();
        renderer.render_frame(ImGui::GetDrawData());
        const auto frame = renderer.readback();
        check(frame.size.width == 160 && frame.size.height == 140, "Renderer kept an obsolete drawable size");
        const auto offset = (static_cast<std::size_t>(70) * frame.size.width + 80) * 3;
        check(frame.rgb[offset] == 230 && frame.rgb[offset + 1] == 30 && frame.rgb[offset + 2] == 40,
              "Preserved image handle did not render after changing decorations");
        renderer.present();
        renderer.update_image(image, {0, 0, 2, 2}, {pixels.data(), pixels.size(), 8, {2, 2}});
    }
    renderer.destroy_image(image);
    check(renderer.capabilities().image_bytes == base, "Decoration switches leaked GPU storage");
    std::puts("PASS in-place decoration switches preserve window, context, handles, pixels, font textures and accounting");
}

} // namespace
int main(int argc, char **)
{
    SDL_Window *window = nullptr;
    bool platform      = false;
    try
    {
        check(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        const auto flags = liltview::prepare_render_window();
        window           = SDL_CreateWindow("Renderer contract", 128, 128, flags | SDL_WINDOW_HIDDEN);
        check(window, SDL_GetError());
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        auto renderer =
            liltview::render::create_renderer({liltview::render::SurfaceKind::sdl_window, window, 1024 * 1024, false, argc == 1});
        platform = liltview::initialize_imgui_platform(window);
        check(platform, "SDL platform backend");
        exercise(*renderer);
        verify_populated_creation(*renderer);
        verify_magnification(*renderer);
        verify_staging(*renderer, argc == 1);
        verify_decoration_switch(*renderer, window);
        verify_opaque_background(*renderer, window);
        renderer.reset();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    } catch (const std::exception &e)
    {
        std::fprintf(stderr, "Renderer contract failed: %s\n", e.what());
        if (platform)
            ImGui_ImplSDL3_Shutdown();
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
}
