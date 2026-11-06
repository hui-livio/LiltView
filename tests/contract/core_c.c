#include <liltview/core.h>

#include <stdckdint.h>
#include <stdio.h>

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
    #error This contract check must compile as C23.
#endif

int main(void)
{
    lv_core_config config;
    lv_core_config_init(&config);
    if (lv_core_abi_version() != LV_CORE_ABI_VERSION || config.struct_size != sizeof(config) || config.cpu_budget_bytes != 0 ||
        config.queue_pixel_bytes != 0)
    {
        fputs("C ABI/config initialization failed\n", stderr);
        return 1;
    }

    lv_request_desc request;
    lv_request_desc_init(&request);
    if (request.struct_size != sizeof(request) || request.target.kind != LV_TARGET_FIT || request.path.data != nullptr)
    {
        fputs("C request initialization failed\n", stderr);
        return 2;
    }

    // The toolchain must reject overflow rather than wrapping an image size.
    constexpr size_t channels = 4;
    size_t bytes              = 0;
    if (!ckd_mul(&bytes, SIZE_MAX, channels))
    {
        fputs("C23 checked arithmetic did not reject overflow\n", stderr);
        return 3;
    }
    lv_core_config_init(nullptr);
    lv_request_desc_init(nullptr);
    return 0;
}
