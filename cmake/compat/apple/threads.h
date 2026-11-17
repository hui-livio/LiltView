// Darwin implementation of the C11 threading subset used by the core.
// SPDX-License-Identifier: Apache-2.0
#ifndef LILTVIEW_APPLE_THREADS_H
#define LILTVIEW_APPLE_THREADS_H
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

typedef pthread_t thrd_t;
typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t cnd_t;
typedef int (*thrd_start_t)(void *);
enum { thrd_success, thrd_error, thrd_nomem, thrd_timedout, thrd_busy };
enum { mtx_plain = 0 };
struct lv_thread_start { thrd_start_t function; void *argument; };
static inline void *lv_thread_entry(void *opaque)
{
    const struct lv_thread_start start = *(struct lv_thread_start *)opaque;
    free(opaque);
    return (void *)(intptr_t)start.function(start.argument);
}
static inline int thrd_create(thrd_t *thread, thrd_start_t function, void *argument)
{
    struct lv_thread_start *start = malloc(sizeof *start);
    if (!start) return thrd_nomem;
    *start = (struct lv_thread_start){function, argument};
    const int error = pthread_create(thread, NULL, lv_thread_entry, start);
    if (error) free(start);
    return error == 0 ? thrd_success : (error == ENOMEM ? thrd_nomem : thrd_error);
}
static inline int thrd_join(thrd_t thread, int *result)
{
    void *value = NULL;
    if (pthread_join(thread, &value)) return thrd_error;
    if (result) *result = (int)(intptr_t)value;
    return thrd_success;
}
static inline int mtx_init(mtx_t *mutex, int type)
{
    if (type != mtx_plain) return thrd_error;
    return pthread_mutex_init(mutex, NULL) == 0 ? thrd_success : thrd_error;
}
static inline void mtx_destroy(mtx_t *mutex) { pthread_mutex_destroy(mutex); }
static inline int mtx_lock(mtx_t *mutex) { return pthread_mutex_lock(mutex) == 0 ? thrd_success : thrd_error; }
static inline int mtx_unlock(mtx_t *mutex) { return pthread_mutex_unlock(mutex) == 0 ? thrd_success : thrd_error; }
static inline int cnd_init(cnd_t *condition) { return pthread_cond_init(condition, NULL) == 0 ? thrd_success : thrd_error; }
static inline void cnd_destroy(cnd_t *condition) { pthread_cond_destroy(condition); }
static inline int cnd_broadcast(cnd_t *condition) { return pthread_cond_broadcast(condition) == 0 ? thrd_success : thrd_error; }
static inline int cnd_wait(cnd_t *condition, mtx_t *mutex)
{
    return pthread_cond_wait(condition, mutex) == 0 ? thrd_success : thrd_error;
}
static inline int cnd_timedwait(cnd_t *condition, mtx_t *mutex, const struct timespec *deadline)
{
    const int error = pthread_cond_timedwait(condition, mutex, deadline);
    return error == 0 ? thrd_success : (error == ETIMEDOUT ? thrd_timedout : thrd_error);
}
#endif
