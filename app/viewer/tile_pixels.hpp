#pragma once
#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>
#include "renderer/renderer.hpp"
namespace liltview::tile_pixels
{
inline std::optional<std::uint32_t> uniform_color(const std::vector<std::uint8_t> &data)
{
    std::uint32_t first;
    std::memcpy(&first, data.data(), 4);
    for (std::size_t at = 4; at < data.size(); at += 4)
    {
        std::uint32_t pixel;
        std::memcpy(&pixel, data.data() + at, 4);
        if (pixel != first)
            return {};
    }
    return first;
}
inline std::vector<std::uint8_t> solid(render::Size size, std::uint32_t color)
{
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size.width) * size.height * 4);
    std::memcpy(data.data(), &color, 4);
    for (std::size_t at = 4; at < data.size();)
    {
        const auto count = std::min(at, data.size() - at);
        std::memcpy(data.data() + at, data.data(), count);
        at += count;
    }
    return data;
}

// Built once during native decode, not on zoom. Alpha-correct box filtering
// supplies texture LODs without keeping a separate full-image thumbnail.
inline std::vector<std::uint8_t> reduce(const std::vector<std::uint8_t> &in, render::Size size)
{
    const render::Size next{(size.width + 1) / 2, (size.height + 1) / 2};
    std::vector<std::uint8_t> out(static_cast<std::size_t>(next.width) * next.height * 4);
    for (unsigned y = 0; y < next.height; ++y)
        for (unsigned x = 0; x < next.width; ++x)
        {
            unsigned alpha = 0, color[3]{}, count = 0;
            for (unsigned dy = 0; dy < 2 && y * 2 + dy < size.height; ++dy)
                for (unsigned dx = 0; dx < 2 && x * 2 + dx < size.width; ++dx)
                {
                    const auto at  = (static_cast<std::size_t>(y * 2 + dy) * size.width + x * 2 + dx) * 4;
                    alpha         += in[at + 3];
                    for (unsigned c = 0; c < 3; ++c)
                        color[c] += static_cast<unsigned>(in[at + c]) * in[at + 3];
                    ++count;
                }
            const auto at = (static_cast<std::size_t>(y) * next.width + x) * 4;
            for (unsigned c = 0; c < 3; ++c)
                out[at + c] = static_cast<std::uint8_t>(alpha ? (color[c] + alpha / 2) / alpha : 0);
            out[at + 3] = static_cast<std::uint8_t>((alpha + count / 2) / count);
        }
    return out;
}
} // namespace liltview::tile_pixels
