// Check the private Darwin C11 threading adapter against the same core contract.
#include <threads.h>
#include <time.h>
struct state { mtx_t mutex; cnd_t condition; int ready; };
static int worker(void *opaque)
{
    struct state *state = opaque;
    if (mtx_lock(&state->mutex) != thrd_success) return 1;
    state->ready = 1;
    cnd_broadcast(&state->condition);
    mtx_unlock(&state->mutex);
    return -42;
}
int main(void)
{
    struct state state = {0};
    thrd_t thread;
    if (mtx_init(&state.mutex, mtx_plain) != thrd_success || cnd_init(&state.condition) != thrd_success) return 1;
    if (mtx_lock(&state.mutex) != thrd_success || thrd_create(&thread, worker, &state) != thrd_success) return 2;
    while (!state.ready)
        if (cnd_wait(&state.condition, &state.mutex) != thrd_success) return 3;
    struct timespec deadline;
    if (timespec_get(&deadline, TIME_UTC) != TIME_UTC) return 4;
    deadline.tv_nsec += 10000000;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_nsec -= 1000000000; ++deadline.tv_sec; }
    if (cnd_timedwait(&state.condition, &state.mutex, &deadline) != thrd_timedout) return 5;
    mtx_unlock(&state.mutex);
    int result = 0;
    if (thrd_join(thread, &result) != thrd_success || result != -42) return 6;
    cnd_destroy(&state.condition);
    mtx_destroy(&state.mutex);
    return 0;
}
