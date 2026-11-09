#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <algorithm>
#include <array>
#include <cstdio>
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

} // namespace
int main()
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
            liltview::render::create_renderer({liltview::render::SurfaceKind::sdl_window, window, 1024 * 1024, false});
        platform = liltview::initialize_imgui_platform(window);
        check(platform, "SDL platform backend");
        exercise(*renderer);
        verify_populated_creation(*renderer);
        verify_magnification(*renderer);
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
