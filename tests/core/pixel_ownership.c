// Internal ownership checks: compare addresses, not just reconstructed colors.
#include <stdio.h>
#include <stdlib.h>
#include "../../core/src/codecs/codec.h"

#define CHECK(expr)                                                                                                              \
    do                                                                                                                           \
    {                                                                                                                            \
        if (!(expr))                                                                                                             \
        {                                                                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr);                                                           \
            exit(1);                                                                                                             \
        }                                                                                                                        \
    } while (0)

static void drain(lv_core *core)
{
    lv_event *event = nullptr;
    while (lv_core_poll_event(core, &event) == LV_OK)
        lv_event_release(event);
}

static void direct_tiles(lv_core *core, lv_request *r, lv_input *input)
{
    lv_error_info error = {0};
    r->desc.target      = (lv_target){.kind = LV_TARGET_REGION, .as.region = {{97, 65}, {13, 7, 35, 31}}};
    lv_frame frame      = {.core = core, .request = r, .input = input};
    CHECK(lv_frame_prepare(&frame, (lv_size_u32){97, 65}, (lv_size_u32){97, 65}, LV_FORMAT_PNG, LV_CAP_REGION_DECODE, 0, &error));
    CHECK(!frame.data && !frame.output && frame.tiles);
    uint32_t width   = 0;
    uint8_t *decoded = lv_frame_span(&frame, 0, 0, &width);
    CHECK(width == 3); // The region starts inside a globally aligned tile.
    decoded[0] = 197;
    CHECK(lv_frame_finish(&frame, &error));
    for (size_t i = 0; i < frame.tile_count; ++i)
        CHECK(frame.tiles[i].pixels == nullptr); // Ownership moved, never duplicated.
    lv_frame_release(&frame);
    lv_frame_release(&frame);
    lv_event *event = nullptr;
    CHECK(lv_core_poll_event(core, &event) == LV_OK);
    lv_pixels *held = lv_event_get_view(event)->as.region.pixels;
    CHECK(lv_pixels_get_view(held)->data == decoded);
    lv_pixels_retain(held);
    lv_event_release(event);
    drain(core);

    lv_frame next = {.core = core, .request = r, .input = input};
    CHECK(lv_frame_prepare(&next, (lv_size_u32){97, 65}, (lv_size_u32){97, 65}, LV_FORMAT_PNG, LV_CAP_REGION_DECODE, 0, &error));
    uint8_t *replacement = lv_frame_span(&next, 0, 0, &width);
    CHECK(replacement != decoded);
    replacement[0] = 11;
    CHECK(decoded[0] == 197); // Retain protects against recycling and overwrite.
    lv_frame_release(&next);  // Cancellation before publication.
    lv_pixels_release(held);
    core->reserved -= r->reservation;
    r->reservation  = 0;
}

static void shared_slices(lv_core *core, lv_request *r, lv_input *input)
{
    lv_error_info error = {0};
    r->desc.target      = (lv_target){.kind = LV_TARGET_FIT, .as.fit_bounds = {97, 65}};
    lv_frame frame      = {.core = core, .request = r, .input = input};
    CHECK(lv_frame_prepare(&frame, (lv_size_u32){97, 65}, (lv_size_u32){97, 65}, LV_FORMAT_WEBP, 0, 0, &error));
    uint8_t *decoded = frame.data;
    decoded[0]       = 149;
    CHECK(lv_frame_finish(&frame, &error));
    lv_event *event = nullptr;
    CHECK(lv_core_poll_event(core, &event) == LV_OK);
    lv_pixels *held           = lv_event_get_view(event)->as.region.pixels;
    const lv_pixel_view *view = lv_pixels_get_view(held);
    CHECK(view->data == decoded && view->stride_bytes == 97 * 4);
    CHECK(view->byte_count == 15 * 97 * 4 + 16 * 4);
    lv_pixels_retain(held);
    lv_event_release(event);
    drain(core);
    lv_frame_release(&frame);
    CHECK(view->data[0] == 149); // Parent outlives decoder frame and every event.
    CHECK(lv_core_begin_shutdown(core) == LV_OK);
    CHECK(view->data[0] == 149); // Shutdown also preserves external references.
    lv_pixels_release(held);
    CHECK(!core->live_pixels && !core->used && !core->reserved);
}

static void reuse_and_pressure(lv_core *core, lv_request *r)
{
    CHECK(lv_reserve(core, r, sizeof(lv_pixels) + 16 * 16 * 4));
    lv_pixels *first = lv_pixels_new(core, r, 16, 16);
    CHECK(first);
    const uint8_t *address = first->view.data;
    mtx_lock(&core->mutex);
    lv_pixels_drop_locked(first, false);
    mtx_unlock(&core->mutex);
    CHECK(lv_reserve(core, r, sizeof(lv_pixels) + 16 * 16 * 4));
    lv_pixels *second = lv_pixels_new(core, r, 16, 16);
    CHECK(second && second->view.data == address);
    mtx_lock(&core->mutex);
    lv_pixels_drop_locked(second, false);
    mtx_unlock(&core->mutex);
    CHECK(core->free_pixel_count);
    // Idle buffers must be reclaimed instead of blocking the sole worker.
    CHECK(lv_reserve(core, r, core->config.cpu_budget_bytes));
    CHECK(!core->free_pixels && !core->used);
    core->reserved -= r->reservation;
    r->reservation  = 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    lv_request request = {.path = argv[1]};
    atomic_init(&request.cancelled, false);
    lv_core core = {.requests = &request};
    lv_core_config_init(&core.config);
    core.config.max_requests           = 1;
    core.config.animation.max_sessions = 0;
    core.config.cpu_budget_bytes       = 4 * 1024 * 1024;
    core.config.queue_pixel_bytes      = 1024 * 1024;
    core.config.tile_edge              = 16;
    CHECK(mtx_init(&core.mutex, mtx_plain) == thrd_success);
    CHECK(cnd_init(&core.condition) == thrd_success);
    uint32_t code  = 0;
    lv_input input = {0};
    input.file     = lv_file_open(argv[1], &input.version, &code);
    CHECK(input.file);
    direct_tiles(&core, &request, &input);
    reuse_and_pressure(&core, &request);
    shared_slices(&core, &request, &input);
    lv_file_close(input.file);
    cnd_destroy(&core.condition);
    mtx_destroy(&core.mutex);
    puts("PASS direct pointer transfer, strided shared ownership, retain isolation, reuse, pressure and shutdown");
    return 0;
}
