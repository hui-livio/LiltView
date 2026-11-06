#include <liltview/core.h>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("configuration initialization resets prior callbacks and budgets", "[config]")
{
    lv_core_config config{};
    config.cpu_budget_bytes        = 1024;
    config.disk_cache_budget_bytes = 1024;
    config.log_userdata            = &config;
    config.wakeup_userdata         = &config;
    config.log = [](void *, lv_log_level, lv_request_id, const char *, size_t) {};
    config.wakeup = [](void *) {};

    lv_core_config_init(&config);

    CHECK(config.abi_version == lv_core_abi_version());
    CHECK(config.struct_size == sizeof(config));
    CHECK(config.cpu_budget_bytes == 0);
    CHECK(config.cache_budget_bytes == 0);
    CHECK(config.queue_pixel_bytes == 0);
    CHECK(config.disk_cache_budget_bytes == 0);
    CHECK(config.disk_cache_directory.data == nullptr);
    CHECK(config.log == nullptr);
    CHECK(config.log_userdata == nullptr);
    CHECK(config.wakeup == nullptr);
    CHECK(config.wakeup_userdata == nullptr);
    CHECK(config.small_file_bytes <= config.small_pixel_bytes);
    CHECK(config.small_pixel_bytes <= config.large_pixel_bytes);
    CHECK(config.tile_edge >= 16);
    CHECK((config.tile_edge & (config.tile_edge - 1)) == 0);
}

TEST_CASE("request initialization clears a prior path and region target", "[request]")
{
    lv_request_desc request{};
    const char path[]        = "/previous.png";
    request.path             = {LV_PATH_UTF8, path, sizeof(path) - 1};
    request.kind             = LV_REQUEST_PREFETCH;
    request.priority         = LV_PRIORITY_PREFETCH;
    request.user_tag         = 42;
    request.target.kind      = LV_TARGET_REGION;
    request.target.as.region = {{12000, 8000}, {5400, 3600, 1200, 800}};

    lv_request_desc_init(&request);

    CHECK(request.struct_size == sizeof(request));
    CHECK(request.kind == LV_REQUEST_DISPLAY);
    CHECK(request.priority == LV_PRIORITY_CURRENT);
    CHECK(request.user_tag == 0);
    CHECK(request.path.data == nullptr);
    CHECK(request.path.byte_count == 0);
    CHECK(request.target.kind == LV_TARGET_FIT);
    CHECK(request.target.as.fit_bounds.width == 0);
    CHECK(request.target.as.fit_bounds.height == 0);
}
