/*
 * Copyright © 2018, VideoLAN and dav1d authors
 * Copyright © 2018, Two Orioles, LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/* Extracted atomic reference lifetime operations from dav1d src/ref.h/ref.c.
 * Allocation/pool/decoder dependencies intentionally omitted; C11 atomics used.
 * Changes: allocation adapters and asserts removed; no payload semantics added.
 */
#ifndef LILTVIEW_DAV1D_REF_H
#define LILTVIEW_DAV1D_REF_H
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
typedef struct Dav1dRef {
    void *data;
    const void *const_data;
    atomic_int ref_cnt;
    int free_ref;
    void (*free_callback)(const uint8_t *data, void *user_data);
    void *user_data;
} Dav1dRef;
static inline Dav1dRef *dav1d_ref_init(Dav1dRef *const ref, const void *const ptr,
    void (*const free_callback)(const uint8_t *, void *), void *const user_data, const int free_ref)
{
    ref->data = NULL;
    ref->const_data = ptr;
    atomic_init(&ref->ref_cnt, 1);
    ref->free_ref = free_ref;
    ref->free_callback = free_callback;
    ref->user_data = user_data;
    return ref;
}
static inline void dav1d_ref_inc(Dav1dRef *const ref) {
    atomic_fetch_add_explicit(&ref->ref_cnt, 1, memory_order_relaxed);
}
static inline void dav1d_ref_dec(Dav1dRef **const pref) {
    Dav1dRef *const ref = *pref;
    if (!ref) return;
    *pref = NULL;
    if (atomic_fetch_sub(&ref->ref_cnt, 1) == 1) {
        const int free_ref = ref->free_ref;
        ref->free_callback(ref->const_data, ref->user_data);
        if (free_ref) free(ref);
    }
}
#endif
