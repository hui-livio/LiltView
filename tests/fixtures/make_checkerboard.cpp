#include "checkerboard.hpp"
int main(int argc, char **argv)
{
    try
    {
        if (argc != 4 && argc != 5)
            throw std::runtime_error("Usage: liltview_make_checkerboard OUTPUT WIDTH HEIGHT [CELL=1024]");
        const auto width = std::stoul(argv[2]), height = std::stoul(argv[3]), cell = argc == 5 ? std::stoul(argv[4]) : 1024ul;
        liltview::test::fixture_check(width <= UINT32_MAX && height <= UINT32_MAX && cell <= UINT32_MAX,
                                      "Geometry exceeds unsigned 32-bit range");
        liltview::test::checkerboard(argv[1], static_cast<unsigned>(width), static_cast<unsigned>(height),
                                     static_cast<unsigned>(cell));
        std::printf("Created %lux%lu red/white checkerboard: %s\n", width, height, argv[1]);
        return 0;
    } catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
