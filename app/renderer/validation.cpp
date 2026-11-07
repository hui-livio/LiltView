#include "renderer.hpp"

#include <limits>

namespace liltview::render
{
void validate_upload(Size image, Rect rect, Pixels source)
{
    constexpr auto limit = std::numeric_limits<std::uint64_t>::max();
    const auto row       = static_cast<std::uint64_t>(source.size.width) * 4;
    if (!source.data || !rect.width || !rect.height || source.size.width != rect.width || source.size.height != rect.height ||
        rect.x > image.width || rect.width > image.width - rect.x || rect.y > image.height ||
        rect.height > image.height - rect.y || source.stride_bytes < row)
        throw Error(ErrorCode::invalid_argument, "Invalid RGBA upload rectangle or stride");
    const auto rows = static_cast<std::uint64_t>(source.size.height - 1);
    if ((rows && source.stride_bytes > (limit - row) / rows) || rows * source.stride_bytes + row > source.byte_count ||
        source.byte_count > std::numeric_limits<std::size_t>::max())
        throw Error(ErrorCode::invalid_argument, "RGBA source is shorter than its declared rows");
}

std::uint64_t image_storage_bytes(ImageDesc desc)
{
    auto width  = static_cast<std::uint64_t>(desc.size.width);
    auto height = static_cast<std::uint64_t>(desc.size.height);
    if (!width || !height || width > std::numeric_limits<std::uint64_t>::max() / 4 / height)
        throw Error(ErrorCode::dimension_limit, "Invalid image dimensions");
    std::uint64_t result = 0;
    for (;;)
    {
        const auto bytes = width * height * 4;
        if (bytes > std::numeric_limits<std::uint64_t>::max() - result)
            throw Error(ErrorCode::dimension_limit, "Image storage overflows");
        result += bytes;
        if (!desc.mipmaps || (width == 1 && height == 1))
            return result;
        width  = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
}
} // namespace liltview::render
