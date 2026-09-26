/* SPDX-License-Identifier: Apache-2.0 */
/*
 * C ABI between code instrumented by the CoreTrace compiler and the instrumentation
 * runtime (ct_instrument_runtime). This header is the single description of that
 * contract:
 *
 *  - the compiler passes take every callee name and LLVM function type from the
 *    prototypes below (src/compilerlib/instrumentation/runtime_abi.hpp);
 *  - the runtime includes it before defining the entry points, so a definition that
 *    drifts from its prototype is a compile error (C-linkage functions cannot be
 *    overloaded), and it takes the address of every entry point of
 *    CT_RUNTIME_ENTRY_POINTS, so a prototype without a definition is a link error of
 *    the runtime's own tests.
 *
 * `site` arguments are interned "file:line:col" strings emitted by the compiler.
 *
 * CT_RUNTIME_ENTRY_POINTS(X) and CT_RUNTIME_OBJC_ENTRY_POINTS(X) expand X(return type,
 * name, parameter list) once per entry point.
 */
#ifndef CORETRACE_RUNTIME_ABI_H
#define CORETRACE_RUNTIME_ABI_H

#include <stddef.h>

/* clang-format off */
#define CT_RUNTIME_ENTRY_POINTS(X)                                                                \
    /* Function entry/exit tracing. */                                                            \
    X(void,  __ct_trace_enter,        (const char* func))                                         \
    X(void,  __ct_trace_exit_void,    (const char* func))                                         \
    X(void,  __ct_trace_exit_i64,     (const char* func, long long value))                        \
    X(void,  __ct_trace_exit_ptr,     (const char* func, const void* value))                      \
    X(void,  __ct_trace_exit_f64,     (const char* func, double value))                           \
    X(void,  __ct_trace_exit_unknown, (const char* func))                                         \
    /* Allocation tracking. The *_unreachable variants record an allocation whose      */        \
    /* result the program never uses; the runtime may release it at once.              */        \
    X(void*, __ct_malloc,             (size_t size, const char* site))                            \
    X(void*, __ct_malloc_unreachable, (size_t size, const char* site))                            \
    X(void*, __ct_calloc,             (size_t count, size_t size, const char* site))              \
    X(void*, __ct_calloc_unreachable, (size_t count, size_t size, const char* site))              \
    X(void*, __ct_realloc,            (void* ptr, size_t size, const char* site))                 \
    X(int,   __ct_posix_memalign,     (void** out, size_t align, size_t size, const char* site))  \
    X(void*, __ct_aligned_alloc,      (size_t align, size_t size, const char* site))              \
    X(void*, __ct_mmap,               (void* addr, size_t len, int prot, int flags, int fd,       \
                                       size_t offset, const char* site))                          \
    X(int,   __ct_munmap,             (void* addr, size_t len, const char* site))                 \
    X(void*, __ct_sbrk,               (size_t incr, const char* site))                            \
    X(void*, __ct_brk,                (void* addr, const char* site))                             \
    X(void*, __ct_new,                          (size_t size, const char* site))                  \
    X(void*, __ct_new_unreachable,              (size_t size, const char* site))                  \
    X(void*, __ct_new_array,                    (size_t size, const char* site))                  \
    X(void*, __ct_new_array_unreachable,        (size_t size, const char* site))                  \
    X(void*, __ct_new_nothrow,                  (size_t size, const char* site))                  \
    X(void*, __ct_new_nothrow_unreachable,      (size_t size, const char* site))                  \
    X(void*, __ct_new_array_nothrow,            (size_t size, const char* site))                  \
    X(void*, __ct_new_array_nothrow_unreachable,(size_t size, const char* site))                  \
    /* Releases receive the site of their call, which a double free reports. */                \
    X(void,  __ct_free,                   (void* ptr, const char* site))                          \
    X(void,  __ct_delete,                 (void* ptr, const char* site))                          \
    X(void,  __ct_delete_array,           (void* ptr, const char* site))                          \
    X(void,  __ct_delete_nothrow,         (void* ptr, const char* site))                          \
    X(void,  __ct_delete_array_nothrow,   (void* ptr, const char* site))                          \
    X(void,  __ct_delete_destroying,      (void* ptr, const char* site))                          \
    X(void,  __ct_delete_array_destroying,(void* ptr, const char* site))                          \
    /* Compile-time proven unreachable allocations, released at function exit. */                \
    X(void,  __ct_autofree,              (void* ptr))                                             \
    X(void,  __ct_autofree_delete,       (void* ptr))                                             \
    X(void,  __ct_autofree_delete_array, (void* ptr))                                             \
    X(void,  __ct_autofree_munmap,       (void* ptr))                                             \
    X(void,  __ct_autofree_sbrk,         (void* ptr))                                             \
    /* Bounds checking. */                                                                        \
    X(void,  __ct_check_bounds, (const void* base, const void* ptr, size_t access_size,           \
                                 const char* site, int is_write))                                 \
    /* Stack objects the checks may use as a base, registered by their frame while it runs. */  \
    /* __ct_stack_push returns the depth __ct_stack_pop restores when the frame exits.      */  \
    X(size_t, __ct_stack_push, (const void* base, size_t size, const char* site))                \
    X(void,  __ct_stack_pop,   (size_t depth))                                                    \
    /* Vtable diagnostics (Itanium ABI on POSIX, MSVC ABI on Windows). */                         \
    X(void,  __ct_vtable_dump, (void* this_ptr, const char* site, const char* static_type))       \
    X(void,  __ct_vcall_trace, (void* this_ptr, void* target, const char* site,                   \
                                const char* static_type))

/* Objective-C object tracking, with the Apple runtime only: the compiler emits these
 * calls for Apple targets alone, and only the Apple build of the runtime defines them.
 * They call into libobjc, so they stay out of the addresses the runtime always links:
 * an instrumented C program must not depend on it. */
#define CT_RUNTIME_OBJC_ENTRY_POINTS(X)                                                           \
    /* `object` has just been returned by an allocation that `cls` received. */                  \
    X(void,  __ct_objc_track, (void* object, const void* cls, const char* site))

/* Configuration globals. The compiler emits every one of them into each instrumented
 * module (weak ODR, value 0 or 1); the runtime imports them weakly and reads them once
 * at initialisation. */
#define CT_RUNTIME_CONFIG_GLOBALS(X)                                                              \
    X(__ct_config_shadow)                                                                         \
    X(__ct_config_shadow_aggressive)                                                              \
    X(__ct_config_bounds_no_abort)                                                                \
    X(__ct_config_disable_alloc)                                                                  \
    X(__ct_config_disable_autofree)                                                               \
    X(__ct_config_disable_alloc_trace)                                                            \
    X(__ct_config_vtable_diag)
/* clang-format on */

#ifdef __cplusplus
extern "C"
{
#endif

#define CT_RUNTIME_DECLARE_ENTRY_POINT(ret, name, params) ret name params;
    CT_RUNTIME_ENTRY_POINTS(CT_RUNTIME_DECLARE_ENTRY_POINT)
    CT_RUNTIME_OBJC_ENTRY_POINTS(CT_RUNTIME_DECLARE_ENTRY_POINT)
#undef CT_RUNTIME_DECLARE_ENTRY_POINT

#define CT_RUNTIME_DECLARE_CONFIG_GLOBAL(name) extern int name;
    CT_RUNTIME_CONFIG_GLOBALS(CT_RUNTIME_DECLARE_CONFIG_GLOBAL)
#undef CT_RUNTIME_DECLARE_CONFIG_GLOBAL

#ifdef __cplusplus
}
#endif

/* Name of a runtime symbol as a string literal. Naming the symbol's type first makes
 * an unknown or misspelt symbol a compile error instead of a silent link failure. */
#ifdef __cplusplus
#define CT_RUNTIME_SYMBOL(sym) ((void)sizeof(decltype(&(sym))), #sym)
#else
#define CT_RUNTIME_SYMBOL(sym) ((void)sizeof(&(sym)), #sym)
#endif

#endif /* CORETRACE_RUNTIME_ABI_H */
