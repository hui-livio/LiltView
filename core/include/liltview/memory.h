#ifndef LILTVIEW_MEMORY_H
#define LILTVIEW_MEMORY_H
#include <liltview/core.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Content-agnostic immutable published byte storage. Initial reference = 1.
 * Retain/release are thread safe. Refcounts do NOT synchronize payload writes.
 * The final release calls destroy exactly once, on the releasing thread.
 * A wrap failure does not take ownership. No allocation/copy of the payload. */
typedef struct lv_memory_block lv_memory_block;
typedef void(LV_CALL *lv_memory_destroy_fn)(void *context, void *data, uint64_t size);
LV_API lv_memory_block *LV_CALL lv_memory_block_wrap(void *data, uint64_t size, lv_memory_destroy_fn destroy, void *context);
LV_API const void *LV_CALL lv_memory_block_data(const lv_memory_block *block);
LV_API uint64_t LV_CALL lv_memory_block_size(const lv_memory_block *block);
LV_API void LV_CALL lv_memory_block_retain(lv_memory_block *block);
LV_API void LV_CALL lv_memory_block_release(lv_memory_block *block);
#ifdef __cplusplus
}
#endif
#endif
