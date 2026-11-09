#ifndef LV_CODEC_H
#define LV_CODEC_H

#include "../internal.h"

// One opened source/version is shared by detection and decoding. No extension routing.
typedef struct lv_input
{
    FILE *file;
    lv_file_version version;
} lv_input;

typedef struct lv_frame_tile
{
    lv_pixels *pixels; // Exclusive mutable ownership until successful publication.
    lv_rect_u32 rect;
    uint64_t id;
} lv_frame_tile;

// canvas is logical. Regional codecs write directly into their final tiles.
// Legacy codecs retain a bounded contiguous output required by their APIs.
typedef struct lv_frame
{
    lv_core *core;
    lv_request *request;
    lv_input *input;
    lv_image_info info;
    uint8_t *data;
    lv_pixels *output; // Owns contiguous output; published slices retain it.
    size_t bytes;
    uint64_t tile_reserve;
    lv_frame_tile *tiles;
    size_t tile_count, tile_columns, next_publish;
    bool streaming;
    lv_error_code failure;
} lv_frame;

typedef struct lv_axis_sample
{
    uint32_t first, second, fraction;
} lv_axis_sample;
typedef struct lv_rows
{
    lv_frame *frame;
    lv_size_u32 source;
    uint8_t *data;
    size_t bytes;
    uint32_t next_y;
} lv_rows;
typedef bool (*lv_read_row_fn)(void *, uint32_t y, uint8_t *rgba, lv_error_info *);
bool lv_target_canvas(lv_request *, lv_size_u32, lv_orientation, lv_size_u32 *, lv_error_info *);
lv_axis_sample lv_sample_axis(uint32_t coordinate, uint32_t source, uint32_t canvas);
uint32_t lv_sample_nearest(uint32_t coordinate, uint32_t source, uint32_t canvas);
bool lv_rows_init(lv_rows *, lv_frame *, lv_size_u32, lv_error_info *);
uint8_t *lv_rows_buffer(lv_rows *, uint32_t y);
bool lv_rows_push(lv_rows *, uint32_t y, lv_error_info *);
void lv_rows_release(lv_rows *);
bool lv_raster_read(lv_frame *, lv_size_u32, lv_read_row_fn, void *, lv_error_info *);

lv_size_u32 lv_fit_size(lv_size_u32, lv_size_u32, lv_orientation);
bool lv_bitmap_size(lv_core *, uint32_t width, uint32_t height, lv_error_info *);
bool lv_frame_prepare(lv_frame *, lv_size_u32 source, lv_size_u32 canvas, lv_format, lv_capabilities, uint64_t scratch,
                      lv_error_info *);
bool lv_frame_info(lv_frame *, lv_error_info *);
bool lv_frame_finish(lv_frame *, lv_error_info *);
bool lv_frame_flush_rows(lv_frame *, uint32_t completed_rows, lv_error_info *);
void lv_frame_release(lv_frame *);
// Local output coordinates. Returns a writable row span ending at a tile edge.
uint8_t *lv_frame_span(lv_frame *, uint32_t x, uint32_t y, uint32_t *width);
bool lv_resample_rgba(lv_core *, lv_request *, const uint8_t *, lv_size_u32, uint8_t *, lv_size_u32);
uint8_t *lv_input_read_all(lv_core *, lv_request *, lv_input *, lv_error_info *);

void lv_decode_jpeg(lv_core *, lv_request *, lv_input *, lv_error_info *);
void lv_decode_png(lv_core *, lv_request *, lv_input *, lv_error_info *);
void lv_decode_webp(lv_core *, lv_request *, lv_input *, lv_error_info *);
void lv_decode_bmp(lv_core *, lv_request *, lv_input *, lv_error_info *);
void lv_decode_svg(lv_core *, lv_request *, lv_input *, lv_error_info *);
void lv_decode_gif(lv_core *, lv_request *, lv_input *, lv_error_info *);
void lv_decode_tiff(lv_core *, lv_request *, lv_input *, lv_error_info *);
#endif
