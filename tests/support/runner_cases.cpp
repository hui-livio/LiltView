#include <catch2/catch_test_macros.hpp>

// Hidden and deliberately NOT discovered into the regular test suite. These
// cases exercise the project runner as a subprocess, including real failures.
TEST_CASE("runner contract skip", "[.][runner_skip]")
{
    SKIP("Intentional skip for runner exit-code contract");
}

TEST_CASE("runner contract pass", "[.][runner_pass]")
{
    CHECK(true);
}

TEST_CASE("runner contract failure", "[.][runner_fail]")
{
    CHECK(false);
}
