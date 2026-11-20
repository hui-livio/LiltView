#ifndef LILTVIEW_RAW_H
#define LILTVIEW_RAW_H

#include <liltview/core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LV_RAW_ABI_VERSION UINT32_C(4)
#define LV_RAW_MAX_PLANES UINT32_C(4)

typedef uint32_t lv_raw_layout;
enum
{
    LV_RAW_LAYOUT_RGB24 = 1,
    LV_RAW_LAYOUT_BGR24 = 2,
    LV_RAW_LAYOUT_RGBA8 = 3,
    LV_RAW_LAYOUT_BGRA8 = 4,
    LV_RAW_LAYOUT_GRAY8 = 5,
    LV_RAW_LAYOUT_I420  = 6,
    LV_RAW_LAYOUT_YV12  = 7,
    LV_RAW_LAYOUT_NV12  = 8,
    LV_RAW_LAYOUT_NV21  = 9,
    LV_RAW_LAYOUT_BAYER8 = 10,
    LV_RAW_LAYOUT_BAYER16 = 11
};

typedef uint32_t lv_raw_row_order;
enum
{
    LV_RAW_TOP_DOWN    = 1,
    LV_RAW_BOTTOM_UP   = 2
};

typedef uint32_t lv_raw_byte_order;
enum
{
    LV_RAW_BYTE_NATIVE = 1,
    LV_RAW_BYTE_LITTLE = 2,
    LV_RAW_BYTE_BIG    = 3
};

typedef uint32_t lv_raw_alpha_mode;
enum
{
    LV_RAW_ALPHA_NONE     = 1,
    LV_RAW_ALPHA_OPAQUE   = 2,
    LV_RAW_ALPHA_STRAIGHT = 3
};

typedef uint32_t lv_raw_color_matrix;
enum
{
    LV_RAW_MATRIX_UNKNOWN = 0,
    LV_RAW_MATRIX_BT601   = 1,
    LV_RAW_MATRIX_BT709   = 2,
    LV_RAW_MATRIX_BT2020  = 3
};

typedef uint32_t lv_raw_color_range;
enum
{
    LV_RAW_RANGE_UNKNOWN = 0,
    LV_RAW_RANGE_FULL    = 1,
    LV_RAW_RANGE_LIMITED = 2
};

typedef uint32_t lv_raw_chroma_siting;
enum
{
    LV_RAW_CHROMA_CENTER  = 1,
    LV_RAW_CHROMA_COSITED = 2
};

typedef uint32_t lv_raw_primaries;
enum
{
    LV_RAW_PRIMARIES_UNKNOWN = 0,
    LV_RAW_PRIMARIES_SRGB    = 1,
    LV_RAW_PRIMARIES_BT709   = 2
};

typedef uint32_t lv_raw_transfer;
enum
{
    LV_RAW_TRANSFER_UNKNOWN = 0,
    LV_RAW_TRANSFER_LINEAR  = 1,
    LV_RAW_TRANSFER_SRGB    = 2
};

typedef uint32_t lv_raw_cfa_pattern;
enum
{
    LV_RAW_CFA_NONE = 0,
    LV_RAW_CFA_RGGB = 1,
    LV_RAW_CFA_BGGR = 2,
    LV_RAW_CFA_GRBG = 3,
    LV_RAW_CFA_GBRG = 4
};

typedef uint32_t lv_raw_demosaic_quality;
enum
{
    LV_RAW_DEMOSAIC_NEAREST  = 1,
    LV_RAW_DEMOSAIC_BILINEAR = 2
};

typedef struct lv_raw_plane
{
    uint64_t offset;
    uint64_t stride;
    uint64_t row_bytes;
    uint32_t height;
    uint32_t reserved;
} lv_raw_plane;

typedef struct lv_raw_layout_desc
{
    uint32_t struct_size;
    lv_raw_layout layout;
    lv_size_u32 size;
    lv_orientation orientation;
    lv_raw_row_order row_order;
    lv_raw_byte_order byte_order;
    lv_raw_alpha_mode alpha;
    uint32_t plane_count;
    uint32_t reserved;
    lv_raw_plane planes[LV_RAW_MAX_PLANES];
} lv_raw_layout_desc;

typedef struct lv_raw_processing
{
    uint32_t struct_size;
    lv_raw_color_matrix matrix;
    lv_raw_color_range range;
    lv_raw_chroma_siting chroma_siting;
    lv_raw_primaries primaries;
    lv_raw_transfer transfer;
    lv_raw_cfa_pattern cfa;
    lv_raw_demosaic_quality demosaic;
    uint32_t cfa_x_origin;
    uint32_t cfa_y_origin;
    uint32_t black_level;
    uint32_t white_level;
    uint32_t red_gain_q16;
    uint32_t green_red_gain_q16;
    uint32_t green_blue_gain_q16;
    uint32_t blue_gain_q16;
    uint32_t reserved[4];
} lv_raw_processing;

typedef struct lv_raw_output
{
    uint32_t struct_size;
    lv_pixel_format pixel_format;
    lv_color_space color_space;
    uint32_t allow_conversion;
    uint32_t reserved[2];
} lv_raw_output;

typedef struct lv_raw_image_desc
{
    uint32_t struct_size;
    lv_raw_layout_desc layout;
    lv_raw_processing processing;
    lv_raw_output output;
} lv_raw_image_desc;

typedef struct lv_raw_request_desc
{
    uint32_t struct_size;
    uint32_t abi_version;
    lv_request_kind kind;
    lv_priority priority;
    uint64_t user_tag;
    lv_path path;
    lv_target target;
    lv_raw_image_desc image;
} lv_raw_request_desc;

LV_API uint32_t LV_CALL lv_raw_abi_version(void);
LV_API void LV_CALL lv_raw_layout_desc_init(lv_raw_layout_desc *out);
LV_API void LV_CALL lv_raw_processing_init(lv_raw_processing *out);
LV_API void LV_CALL lv_raw_output_init(lv_raw_output *out);
LV_API void LV_CALL lv_raw_image_desc_init(lv_raw_image_desc *out);
LV_API void LV_CALL lv_raw_request_desc_init(lv_raw_request_desc *out);
LV_API lv_status LV_CALL lv_core_submit_raw(lv_core *core, const lv_raw_request_desc *request,
                                            lv_request_id *out_request_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LILTVIEW_RAW_H */
