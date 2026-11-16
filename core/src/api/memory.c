#include <liltview/memory.h>
#include <dav1d-ref/ref.h>
struct lv_memory_block
{
    Dav1dRef ref; // First member: dav1d final decrement frees this allocation.
    void *data;
    uint64_t size;
    lv_memory_destroy_fn destroy;
    void *context;
};
static void destroy_block(const uint8_t *data, void *context)
{
    lv_memory_block *block = context;
    (void)data;
    block->destroy(block->context, block->data, block->size);
}
lv_memory_block *LV_CALL lv_memory_block_wrap(void *data, uint64_t size, lv_memory_destroy_fn destroy, void *context)
{
    if (!data || !size || !destroy) return nullptr;
    lv_memory_block *block = malloc(sizeof *block);
    if (!block) return nullptr;
    block->data = data;
    block->size = size;
    block->destroy = destroy;
    block->context = context;
    dav1d_ref_init(&block->ref, data, destroy_block, block, 1);
    return block;
}
const void *LV_CALL lv_memory_block_data(const lv_memory_block *block) { return block ? block->data : nullptr; }
uint64_t LV_CALL lv_memory_block_size(const lv_memory_block *block) { return block ? block->size : 0; }
void LV_CALL lv_memory_block_retain(lv_memory_block *block) { if (block) dav1d_ref_inc(&block->ref); }
void LV_CALL lv_memory_block_release(lv_memory_block *block)
{
    if (block) { Dav1dRef *ref = &block->ref; dav1d_ref_dec(&ref); }
}
