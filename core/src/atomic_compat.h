/**
 * @file atomic_compat.h
 * @brief Cross-platform atomic operations compatibility.
 *
 * MSVC does not support C11 <stdatomic.h>.  On MSVC we use
 * <intrin.h> Interlocked intrinsics directly, with sizeof-based
 * macro dispatch for 32-bit vs 64-bit operations (pure C, no
 * C++ overloading required).
 *
 * On GCC/Clang, standard C11 <stdatomic.h> is used directly.
 */

#ifndef ATOMIC_COMPAT_H
#define ATOMIC_COMPAT_H

#if defined(_MSC_VER)
/* ═══════════════════════════════════════════════════════════════════ */
/* MSVC: Interlocked intrinsics only -- NO <atomic>, NO <stdatomic.h> */
/* ═══════════════════════════════════════════════════════════════════ */

#include <intrin.h>

/* ─── Atomic types (all volatile for compiler-enforced ordering) ── */
typedef volatile long      atomic_int;
typedef volatile long      atomic_uint;
typedef volatile long      atomic_bool;
typedef volatile __int64   atomic_uint_fast64_t;

/* ─── Memory order constants (ignored -- all ops are full-fence) ── */
#define memory_order_relaxed  0
#define memory_order_acquire  0
#define memory_order_release  0
#define memory_order_seq_cst  0

/* ─── Init / Store / Load ─────────────────────────────────────── */
#define atomic_init(ptr, val)   (void)(*(ptr) = (val))
#define atomic_store(ptr, val)  (void)(*(ptr) = (val))
#define atomic_load(ptr)        (*(ptr))

/* ─── Named helper functions (no overloading -- pure C) ────────── */

static __forceinline long _ae_fetch_add32(volatile long *p, long v)
{ return _InterlockedExchangeAdd(p, v); }

static __forceinline __int64 _ae_fetch_add64(volatile __int64 *p, __int64 v)
{ return _InterlockedExchangeAdd64(p, v); }

static __forceinline long _ae_fetch_sub32(volatile long *p, long v)
{ return _InterlockedExchangeAdd(p, -v); }

static __forceinline __int64 _ae_fetch_sub64(volatile __int64 *p, __int64 v)
{ return _InterlockedExchangeAdd64(p, -v); }

static __forceinline long _ae_exchange32(volatile long *p, long v)
{ return _InterlockedExchange(p, v); }

static __forceinline __int64 _ae_exchange64(volatile __int64 *p, __int64 v)
{ return _InterlockedExchange64(p, v); }

/* ─── sizeof-based dispatch macros (works in C and C++) ────────── */

#define atomic_fetch_add(ptr, val) \
    (sizeof(*(ptr)) == 8 \
        ? _ae_fetch_add64((volatile __int64*)(ptr), (__int64)(val)) \
        : _ae_fetch_add32((volatile long*)(ptr), (long)(val)))

#define atomic_fetch_sub(ptr, val) \
    (sizeof(*(ptr)) == 8 \
        ? _ae_fetch_sub64((volatile __int64*)(ptr), (__int64)(val)) \
        : _ae_fetch_sub32((volatile long*)(ptr), (long)(val)))

#define _ae_exchange_dispatch(ptr, val) \
    (sizeof(*(ptr)) == 8 \
        ? _ae_exchange64((volatile __int64*)(ptr), (__int64)(val)) \
        : _ae_exchange32((volatile long*)(ptr), (long)(val)))

/* ─── Explicit variants (memory order argument is ignored) ─────── */
#define atomic_store_explicit(ptr, val, order)      atomic_store(ptr, val)
#define atomic_load_explicit(ptr, order)            atomic_load(ptr)
#define atomic_fetch_add_explicit(ptr, val, order)  atomic_fetch_add(ptr, val)
#define atomic_fetch_sub_explicit(ptr, val, order)  atomic_fetch_sub(ptr, val)
#define atomic_exchange_explicit(ptr, val, order)   _ae_exchange_dispatch(ptr, val)

#define ATOMIC_VAR_INIT(val) (val)

#else
/* ═══════════════════════════════════════════════════════════════════ */
/* GCC / Clang: standard C11 atomics                                  */
/* ═══════════════════════════════════════════════════════════════════ */

#include <stdatomic.h>

#endif /* _MSC_VER */

#endif /* ATOMIC_COMPAT_H */
