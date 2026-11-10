#include <catch2/catch_session.hpp>
#include <cstdio>

int main(int argc, char *argv[])
{
    const int result = Catch::Session().run(argc, argv);
    // Catch2 3.8.1 uses 4 ONLY when every selected test case was skipped.
    // Preserve real failures, bad arguments and empty/unmatched selections.
    constexpr int all_tests_skipped = 4;
    if (result == all_tests_skipped)
    {
        // Keep reporter stdout (especially XML/JSON) intact. CTest consumes this
        // marker to retain the Skipped status even though direct runners get 0.
        std::fputs("LILTVIEW_ALL_TESTS_SKIPPED\n", stderr);
        return 0;
    }
    return result;
}
