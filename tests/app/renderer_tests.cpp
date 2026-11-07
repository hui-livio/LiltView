#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include "renderer/renderer.hpp"

TEST_CASE("RGBA uploads validate pitch, bounds and short buffers", "[renderer]")
{
    using namespace liltview::render;
    std::array<std::uint8_t, 36> data{};
    Pixels pixels{data.data(), 36, 20, {4, 2}};
    CHECK_NOTHROW(validate_upload({8, 8}, {2, 3, 4, 2}, pixels));
    pixels.byte_count = 35;
    CHECK_THROWS_AS(validate_upload({8, 8}, {2, 3, 4, 2}, pixels), Error);
    pixels.byte_count   = 36;
    pixels.stride_bytes = 15;
    CHECK_THROWS_AS(validate_upload({8, 8}, {2, 3, 4, 2}, pixels), Error);
    pixels.stride_bytes = 20;
    CHECK_THROWS_AS(validate_upload({8, 8}, {7, 3, 4, 2}, pixels), Error);
    pixels.stride_bytes = std::numeric_limits<std::uint64_t>::max();
    CHECK_THROWS_AS(validate_upload({8, 8}, {2, 3, 4, 2}, pixels), Error);
}
TEST_CASE("renderer budgets include complete mip chains and reject overflow", "[renderer]")
{
    using namespace liltview::render;
    CHECK(image_storage_bytes({{4, 4}, false}) == 64);
    CHECK(image_storage_bytes({{4, 4}, true}) == 84);
    CHECK_THROWS_AS(image_storage_bytes({{0, 4}, false}), Error);
    CHECK_THROWS_AS(image_storage_bytes({{UINT32_MAX, UINT32_MAX}, false}), Error);
}
