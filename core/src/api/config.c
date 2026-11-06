#include <liltview/core.h>

#include <limits.h>

static_assert(sizeof(lv_core_config) <= UINT32_MAX);
static_assert(sizeof(lv_request_desc) <= UINT32_MAX);

uint32_t LV_CALL lv_core_abi_version(void)
{
    return LV_CORE_ABI_VERSION;
}

void LV_CALL lv_core_config_init(lv_core_config *out_config)
{
    if (out_config == nullptr)
    {
        return;
    }

    constexpr uint64_t mib           = UINT64_C(1024) * UINT64_C(1024);
    *out_config                      = (lv_core_config){0};
    out_config->abi_version          = LV_CORE_ABI_VERSION;
    out_config->struct_size          = (uint32_t)sizeof(*out_config);
    out_config->small_file_bytes     = UINT64_C(16) * mib;
    out_config->small_pixel_bytes    = UINT64_C(64) * mib;
    out_config->large_pixel_bytes    = UINT64_C(256) * mib;
    out_config->progressive_after_ms = 100;
    out_config->update_interval_ms   = 33;
    out_config->tile_edge            = 256;
    out_config->max_requests         = 16;
    out_config->foreground_workers   = 1;
    out_config->prefetch_workers     = 1;
    out_config->log_level            = LV_LOG_WARN;
}

void LV_CALL lv_request_desc_init(lv_request_desc *out_request)
{
    if (out_request == nullptr)
    {
        return;
    }

    *out_request             = (lv_request_desc){0};
    out_request->struct_size = (uint32_t)sizeof(*out_request);
    out_request->kind        = LV_REQUEST_DISPLAY;
    out_request->priority    = LV_PRIORITY_CURRENT;
    out_request->target.kind = LV_TARGET_FIT;
}
