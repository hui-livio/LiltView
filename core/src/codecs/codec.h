#ifndef LV_CODEC_H
#define LV_CODEC_H

#include "../internal.h"

// One opened source/version is shared by detection and decoding. No extension routing.
typedef struct lv_input
{
    FILE *file;
    lv_file_version version;
} lv_input;

// Ordinary images only: reserve the FIT image and all eventual output tiles.
// Codec scratch/foreign allowances are reserved separately before use.
typedef struct lv_frame
{
    lv_core *core;
    lv_request *request;
    lv_input *input;
    lv_image_info info;
    uint8_t *data;
    size_t bytes;
    uint64_t tile_reserve;
} lv_frame;

lv_size_u32 lv_fit_size(lv_size_u32, lv_size_u32, lv_orientation);
bool lv_bitmap_size(lv_core *, uint32_t width, uint32_t height, lv_error_info *);
bool lv_frame_prepare(lv_frame *, lv_size_u32 source, lv_size_u32 canvas, lv_format, lv_capabilities, uint64_t scratch,
                      lv_error_info *);
bool lv_frame_info(lv_frame *, lv_error_info *);
bool lv_frame_finish(lv_frame *, lv_error_info *);
void lv_frame_release(lv_frame *);
bool lv_publish_rgba(lv_core *, lv_request *, const uint8_t *, lv_size_u32, lv_error_info *);
bool lv_resample_rgba(lv_core *, lv_request *, const uint8_t *, lv_size_u32, uint8_t *, lv_size_u32);
uint8_t *lv_input_read_all(lv_core *, lv_request *, lv_input *, lv_error_info *);

void lv_decode_jpeg(lv_core *, lv_request *, lv_input *, lv_error_info *);
void lv_decode_png(lv_core *, lv_request *, lv_input *, lv_error_info *);
void lv_decode_webp(lv_core *, lv_request *, lv_input *, lv_error_info *);
void lv_decode_bmp(lv_core *, lv_request *, lv_input *, lv_error_info *);
void lv_decode_svg(lv_core *, lv_request *, lv_input *, lv_error_info *);
#endif
