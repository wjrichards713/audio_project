/**
 * @file atomic_compat.h
 * @brief Cross-platform atomic operations compatibility.
 *
 * MSVC does not support C11 <stdatomic.h>, and its <atomic> C++ header
 * internally includes the broken C11 header.  So on MSVC we use
 * <intrin.h> Interlocked intrinsics directly.
 *
 * Requires /TP flag on MSVC (compile .c files as C++) for overloaded
 * inline functions.
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

/* ─── Overloaded Interlocked wrappers (32-bit and 64-bit) ─────── */

static __forceinline long _ae_fetch_add(volatile long *p, long v)
{ return _InterlockedExchangeAdd(p, v); }

static __forceinline __int64 _ae_fetch_add(volatile __int64 *p, __int64 v)
{ return _InterlockedExchangeAdd64(p, v); }

static __forceinline long _ae_fetch_sub(volatile long *p, long v)
{ return _InterlockedExchangeAdd(p, -v); }

static __forceinline __int64 _ae_fetch_sub(volatile __int64 *p, __int64 v)
{ return _InterlockedExchangeAdd64(p, -v); }

static __forceinline long _ae_exchange(volatile long *p, long v)
{ return _InterlockedExchange(p, v); }

static __forceinline __int64 _ae_exchange(volatile __int64 *p, __int64 v)
{ return _InterlockedExchange64(p, v); }

/* ─── Macro API matching C11 <stdatomic.h> names ─────────────── */
#define atomic_fetch_add(ptr, val)  _ae_fetch_add((ptr), (val))
#define atomic_fetch_sub(ptr, val)  _ae_fetch_sub((ptr), (val))

/* Explicit variants (memory order argument is ignored) */
#define atomic_store_explicit(ptr, val, order)      atomic_store(ptr, val)
#define atomic_load_explicit(ptr, order)            atomic_load(ptr)
#define atomic_fetch_add_explicit(ptr, val, order)  atomic_fetch_add(ptr, val)
#define atomic_fetch_sub_explicit(ptr, val, order)  atomic_fetch_sub(ptr, val)
#define atomic_exchange_explicit(ptr, val, order)   _ae_exchange((ptr), (long)(val))

#define ATOMIC_VAR_INIT(val) (val)

#else
/* ═══════════════════════════════════════════════════════════════════ */
/* GCC / Clang: standard C11 atomics                                  */
/* ═══════════════════════════════════════════════════════════════════ */

#include <stdatomic.h>

#endif /* _MSC_VER */

#endif /* ATOMIC_COMPAT_H */
