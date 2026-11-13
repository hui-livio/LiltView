#include <liltview/memory.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
static atomic_int released;
static void LV_CALL destroy(void *context, void *data, uint64_t size)
{
    if (context != &released || size != 64) abort();
    free(data);
    atomic_fetch_add(&released, 1);
}
static int use(void *context)
{
    lv_memory_block *block = context;
    for (unsigned i = 0; i < 10000; ++i)
    {
        lv_memory_block_retain(block);
        if (lv_memory_block_size(block) != 64 || !lv_memory_block_data(block)) abort();
        lv_memory_block_release(block);
    }
    return 0;
}
int main(void)
{
    void *data = malloc(64);
    lv_memory_block *block = lv_memory_block_wrap(data, 64, destroy, &released);
    if (!block || lv_memory_block_data(block) != data) return 1;
    thrd_t workers[4];
    for (unsigned i = 0; i < 4; ++i) if (thrd_create(workers + i, use, block) != thrd_success) abort();
    for (unsigned i = 0; i < 4; ++i) thrd_join(workers[i], nullptr);
    if (atomic_load(&released)) return 2;
    lv_memory_block_release(block);
    if (atomic_load(&released) != 1) return 3;
    lv_memory_block_retain(nullptr);
    lv_memory_block_release(nullptr);
    if (lv_memory_block_data(nullptr) || lv_memory_block_size(nullptr)) return 4;
    puts("PASS C shared pointer ownership and concurrent final release");
    return 0;
}
