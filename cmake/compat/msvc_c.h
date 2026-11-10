#ifndef LILTVIEW_MSVC_C_H
#define LILTVIEW_MSVC_C_H

/* Private to LiltView C translation units, not part of the public ABI.
 * MSVC lacks these C23 spellings. Our constexpr uses are local scalar constants,
 * so const is sufficient; no constexpr arrays or constant-expression API relies
 * on this shim. Do not use this header for C++ or third-party sources. */
#if !defined(_MSC_VER) || defined(__cplusplus)
#error This compatibility header is only for MSVC C targets.
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>
#define nullptr NULL
#define constexpr const
#define static_assert(condition) _Static_assert(condition, #condition)

/* Typed size_t checked arithmetic only, not a replacement for generic C23
 * <stdckdint.h>. Unsigned multiplication stores the low bits on overflow. */
static inline bool lv_msvc_size_mul(size_t *out, size_t left, size_t right)
{
    *out = left * right;
    return right != 0 && left > SIZE_MAX / right;
}

#endif
