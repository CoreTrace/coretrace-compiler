// SPDX-License-Identifier: Apache-2.0
#ifndef CT_RUNTIME_INTERNAL_H
#define CT_RUNTIME_INTERNAL_H

#include "compilerlib/attributes.hpp"
#include "coretrace/runtime_abi.h"

#include <coretrace/logger.hpp>

#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <format>
#include <source_location>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <sched.h>
#include <unistd.h>
#endif

#if defined(_MSC_VER)
#define CT_NOINSTR

#ifndef __ATOMIC_ACQUIRE
#define __ATOMIC_ACQUIRE 2
#endif
#ifndef __ATOMIC_RELEASE
#define __ATOMIC_RELEASE 3
#endif
#ifndef __ATOMIC_ACQ_REL
#define __ATOMIC_ACQ_REL 4
#endif

inline int ct_msvc_atomic_exchange(volatile int* object, int desired)
{
    return static_cast<int>(
        InterlockedExchange(reinterpret_cast<volatile long*>(object), static_cast<long>(desired)));
}

inline void ct_msvc_atomic_store(volatile int* object, int desired)
{
    (void)InterlockedExchange(reinterpret_cast<volatile long*>(object), static_cast<long>(desired));
}

inline bool ct_msvc_atomic_compare_exchange(volatile int* object, int* expected, int desired)
{
    const long prior =
        InterlockedCompareExchange(reinterpret_cast<volatile long*>(object),
                                   static_cast<long>(desired), static_cast<long>(*expected));
    if (prior == static_cast<long>(*expected))
    {
        return true;
    }

    *expected = static_cast<int>(prior);
    return false;
}

#define __atomic_exchange_n(object, desired, order)                                                \
    ct_msvc_atomic_exchange(reinterpret_cast<volatile int*>(object), static_cast<int>(desired))
#define __atomic_store_n(object, desired, order)                                                   \
    ct_msvc_atomic_store(reinterpret_cast<volatile int*>(object), static_cast<int>(desired))
#define __atomic_compare_exchange_n(object, expected, desired, weak, success_order, failure_order) \
    ct_msvc_atomic_compare_exchange(reinterpret_cast<volatile int*>(object),                       \
                                    reinterpret_cast<int*>(expected), static_cast<int>(desired))
#else
#define CT_NOINSTR __attribute__((no_instrument_function))
#endif

// Takes a lock word the runtime spins on, rather than a mutex, so that it can be taken
// where blocking is not allowed. A waiter spins a few times, then yields the processor
// between attempts: with more threads than processors, a waiter that only spins can keep
// the holder from running, and every thread waits for whole time slices.
CT_NOINSTR inline void ct_spin_lock_acquire(int* lock)
{
    constexpr unsigned kSpinsBeforeYield = 64;
    for (unsigned attempt = 1; __atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) != 0; ++attempt)
    {
        if (attempt >= kSpinsBeforeYield)
        {
#if defined(_WIN32)
            SwitchToThread();
#else
            sched_yield();
#endif
        }
    }
}

using CTColor = coretrace::Color;
using CTLevel = coretrace::Level;

enum
{
    CT_ENTRY_EMPTY = 0,
    CT_ENTRY_USED = 1,
    CT_ENTRY_TOMB = 2,
    CT_ENTRY_FREED = 3,
    CT_ENTRY_AUTOFREED = 4
};

extern int ct_disable_trace;
extern int ct_disable_alloc;
extern int ct_disable_bounds;
extern int ct_bounds_abort;
extern int ct_shadow_enabled;
extern int ct_shadow_aggressive;
extern int ct_autofree_enabled;
extern int ct_alloc_trace_enabled;
extern int ct_vtable_diag_enabled;
extern int ct_alloc_disabled_by_config;
extern int ct_alloc_disabled_by_env;
extern int ct_early_trace;
extern size_t ct_early_trace_count;
extern size_t ct_early_trace_limit;
extern thread_local const char* ct_current_site;
// How many scopes of runtime work this thread is in (CtRuntimeScope).
extern thread_local unsigned ct_runtime_depth;

#define CT_FEATURE_TRACE (1ull << 0)
#define CT_FEATURE_ALLOC (1ull << 1)
#define CT_FEATURE_BOUNDS (1ull << 2)
#define CT_FEATURE_SHADOW (1ull << 3)
#define CT_FEATURE_SHADOW_AGGR (1ull << 4)
#define CT_FEATURE_AUTOFREE (1ull << 5)
#define CT_FEATURE_ALLOC_TRACE (1ull << 6)
#define CT_FEATURE_VTABLE_DIAG (1ull << 7)
#define CT_FEATURE_EARLY_TRACE (1ull << 8)

extern "C"
{
    CT_NODISCARD CT_NOINSTR int ct_is_enabled(uint64_t feature);
    CT_NOINSTR void ct_set_enabled(uint64_t feature, int enabled);
    CT_NODISCARD CT_NOINSTR uint64_t ct_get_features(void);

    CT_NODISCARD CT_NOINSTR int ct_bounds_abort_enabled(void);
    CT_NOINSTR void ct_set_bounds_abort(int enabled);

    CT_NODISCARD CT_NOINSTR int ct_early_trace_should_log(void);
}

CT_NODISCARD CT_NOINSTR size_t ct_strlen(const char* str);
CT_NODISCARD CT_NOINSTR int ct_streq(const char* lhs, const char* rhs);
CT_NODISCARD CT_NOINSTR const char* ct_site_name(const char* site);
CT_NOINSTR void ct_maybe_install_backtrace(void);
CT_NOINSTR void ct_init_env_once(void);
CT_NOINSTR void ct_lock_acquire(void);
CT_NOINSTR void ct_lock_release(void);
CT_NOINSTR void ct_shadow_lock_acquire(void);
CT_NOINSTR void ct_shadow_lock_release(void);
// Held while the runtime writes its log output to stderr: fork() waits on it, so that no other
// thread is writing when the child is created (#133). Never held while calling into the
// logger, and never taken from inside it: the logger's fork() handlers take its own locks, in
// an order relative to the runtime's handlers that neither controls.
CT_NOINSTR void ct_log_lock_acquire(void);
CT_NOINSTR void ct_log_lock_release(void);
// Held while the auto-free scan installs its thread hook, and by fork(), so that a child never
// inherits the hook installed but not yet linked to the previous one (#209). It nests in no
// other lock. No-ops where the scan does not exist.
CT_NOINSTR void ct_autofree_scan_hook_lock_acquire(void);
CT_NOINSTR void ct_autofree_scan_hook_lock_release(void);
// Whether this thread is forking and holds every lock of the runtime, from its fork() prepare
// handler until its parent or child handler. The handlers registered before the runtime's, the
// system's among them, run in between on this thread: the functions that take and release the
// runtime's locks then do nothing, so that whatever these handlers call into the runtime does
// not wait on them (#224).
#if defined(_WIN32)
// Windows has no fork().
CT_NODISCARD CT_NOINSTR inline bool ct_runtime_locked_for_fork(void)
{
    return false;
}
#else
// Set by the runtime's fork() handlers (ct_runtime_env.cpp). Inline, so that the lock functions,
// which read it at every allocation, read it in place.
inline thread_local bool ct_runtime_fork_locked = false;

CT_NODISCARD CT_NOINSTR inline bool ct_runtime_locked_for_fork(void)
{
    return ct_runtime_fork_locked;
}
#endif
CT_NODISCARD CT_NOINSTR int ct_table_insert(void* ptr, size_t req_size, size_t size,
                                            const char* site, unsigned char kind);
CT_NODISCARD CT_NOINSTR int ct_table_remove(void* ptr, size_t* size_out, size_t* req_size_out,
                                            const char** site_out);
CT_NODISCARD CT_NOINSTR int ct_table_lookup(const void* ptr, size_t* size_out, size_t* req_size_out,
                                            const char** site_out, unsigned char* state_out);
CT_NODISCARD CT_NOINSTR int ct_table_lookup_containing(const void* ptr, void** base_out,
                                                       size_t* size_out, size_t* req_size_out,
                                                       const char** site_out,
                                                       unsigned char* state_out);
// The index of the table's blocks by address (ct_runtime_block_index.h), which
// ct_table_lookup_containing goes through. ct_alloc_lock held.
CT_NODISCARD CT_NOINSTR struct ct_block_index* ct_alloc_index(void);
CT_NOINSTR void ct_shadow_poison_range(const void* addr, size_t size);
CT_NOINSTR void ct_shadow_unpoison_range(const void* addr, size_t size);
CT_NODISCARD CT_NOINSTR int ct_shadow_check_access(const void* ptr, size_t access_size,
                                                   const void* base, size_t req_size,
                                                   size_t alloc_size, const char* alloc_site,
                                                   const char* site, int is_write,
                                                   unsigned char state);
CT_NOINSTR void ct_report_bounds_error(const void* base, const void* ptr, size_t access_size,
                                       const char* site, int is_write, size_t req_size,
                                       size_t alloc_size, const char* alloc_site,
                                       unsigned char state);

CT_NODISCARD CT_NOINSTR inline std::string_view ct_color(CTColor color)
{
    return coretrace::color(color);
}

CT_NODISCARD CT_NOINSTR inline std::string_view ct_level_label(CTLevel level)
{
    return coretrace::level_label(level);
}

CT_NODISCARD CT_NOINSTR inline std::string_view ct_level_color(CTLevel level)
{
    return coretrace::level_color(level);
}

CT_NODISCARD CT_NOINSTR inline int ct_pid(void)
{
    return coretrace::pid();
}

CT_NODISCARD CT_NOINSTR inline unsigned long long ct_thread_id(void)
{
    return coretrace::thread_id();
}

CT_NODISCARD CT_NOINSTR inline int ct_log_is_enabled(void)
{
    return coretrace::log_is_enabled() ? 1 : 0;
}

// The logger's output, as the runtime routes it: into the log block this thread is filling,
// or to stderr (see CtLogBlock).
CT_NOINSTR void ct_log_sink(const char* data, size_t size);
// What the writer does when stderr is non-blocking and full.
enum class CtStderrFull
{
    // Waits until it takes more: the output of a running program.
    Wait,
    // Gives up on the rest: the output of a signal handler, which must let the process end.
    GiveUp
};
// Writes every byte to stderr, in order, through partial writes and interrupted calls; when
// a non-blocking stderr is full, as `when_full` says. Gives up when the descriptor fails
// otherwise.
CT_NOINSTR void ct_write_stderr(const char* data, size_t size,
                                CtStderrFull when_full = CtStderrFull::Wait);
// From now on, what this thread logs gives up on a full stderr instead of waiting: for a
// signal handler that does not return, and must let the process end, reported or not.
CT_NOINSTR void ct_log_stop_waiting_for_stderr(void);

CT_NOINSTR inline void ct_enable_logging(void)
{
#if !defined(_WIN32)
    // The runtime serialises its output itself: each line goes to its thread's CtLogBlock, and
    // every write of a block to stderr is under ct_log_lock. The logger's own serialisation
    // would call ct_log_sink under the logger's output lock, which its fork() handlers take:
    // ct_log_sink, which may take ct_log_lock, must not run with it held.
    coretrace::set_thread_safe(false);
#endif
    coretrace::set_sink(ct_log_sink);
    coretrace::enable_logging();
}

CT_NOINSTR inline void ct_disable_logging(void)
{
    coretrace::disable_logging();
}

CT_NOINSTR inline void ct_write_raw(const char* data, size_t size)
{
    coretrace::write_raw(data, size);
}

CT_NOINSTR inline void ct_write_str(std::string_view str)
{
    coretrace::write_str(str);
}

CT_NOINSTR inline void ct_write_cstr(const char* str)
{
    if (!str)
    {
        return;
    }
    coretrace::write_str(std::string_view(str));
}

CT_NOINSTR inline void ct_write_dec(size_t value)
{
    coretrace::write_dec(value);
}

CT_NOINSTR inline void ct_write_hex(uintptr_t value)
{
    coretrace::write_hex(value);
}

CT_NOINSTR inline void ct_write_prefix(CTLevel level)
{
    coretrace::write_prefix(level);
}

// Prefix writer for process teardown and signal handlers. coretrace::write_prefix reads
// the configurable tag under a mutex that may already be destroyed when a global
// destructor runs, and a mutex is never safe inside a signal handler. This variant only
// uses lock-free logger helpers and the runtime's fixed "==ct==" tag, so its output
// matches the regular prefix as long as the runtime never calls coretrace::set_prefix.
CT_NOINSTR inline void ct_write_prefix_nolock(CTLevel level)
{
    constexpr std::string_view tag = "==ct==";

    ct_write_str(ct_color(CTColor::Dim));
    ct_write_raw("|", 1);
    ct_write_dec(static_cast<size_t>(ct_pid()));
    ct_write_raw("|", 1);
    ct_write_str(ct_color(CTColor::Reset));
    ct_write_raw(" ", 1);

    ct_write_str(ct_color(CTColor::Gray));
    ct_write_str(ct_color(CTColor::Italic));
    ct_write_str(tag);
    ct_write_raw(" ", 1);
    ct_write_str(ct_color(CTColor::Reset));

    ct_write_str(ct_level_color(level));
    ct_write_raw("[", 1);
    ct_write_str(ct_level_label(level));
    ct_write_raw("]", 1);
    ct_write_str(ct_color(CTColor::Reset));
    ct_write_raw(" ", 1);
}

// Runtime work on this thread, for its lifetime; scopes nest. The program's copies of inline
// library functions are instrumented, and the linker may keep them for the runtime too: on
// Windows, the MSVC standard library is in headers only, so an allocation the runtime makes
// for itself, a string it formats for instance, reaches its allocation entry points (#197).
// Inside a scope, those entry points pass it to the allocator without tracking or tracing it.
struct CtRuntimeScope
{
    CT_NOINSTR CtRuntimeScope() noexcept
    {
        ++ct_runtime_depth;
    }
    CT_NOINSTR ~CtRuntimeScope()
    {
        --ct_runtime_depth;
    }
    CtRuntimeScope(const CtRuntimeScope&) = delete;
    CtRuntimeScope& operator=(const CtRuntimeScope&) = delete;
};

CT_NODISCARD CT_NOINSTR inline bool ct_in_runtime_scope(void)
{
    return ct_runtime_depth != 0;
}

struct CtLogLockGuard
{
    CT_NOINSTR CtLogLockGuard()
    {
        ct_log_lock_acquire();
    }
    CT_NOINSTR ~CtLogLockGuard()
    {
        ct_log_lock_release();
    }
    CtLogLockGuard(const CtLogLockGuard&) = delete;
    CtLogLockGuard& operator=(const CtLogLockGuard&) = delete;
};

// The lines this thread logs while a block lives, written to stderr at once when the
// outermost block of the thread ends: a box is one write, which no line of another thread
// can split. The lines are the logger's, prefix included. Blocks nest; the inner ones join
// the outermost.
class CtLogBlock
{
  public:
    CT_NOINSTR CtLogBlock();
    CT_NOINSTR ~CtLogBlock();
    CtLogBlock(const CtLogBlock&) = delete;
    CtLogBlock& operator=(const CtLogBlock&) = delete;

  private:
    // The buffer is allocated and freed as runtime work (#197).
    CtRuntimeScope scope_;
    std::string buffer_;
    bool outermost_;
};

// Forgets the block this thread is filling, unwritten, so that what follows goes straight to
// stderr: for a signal handler that does not return to the code filling it.
CT_NOINSTR void ct_log_block_abandon(void);

template <typename... Args>
CT_NOINSTR inline void ct_log(CTLevel level, std::string_view fmt, Args&&... args)
{
    if (!coretrace::log_is_enabled())
    {
        return;
    }

    CtRuntimeScope scope;
    try
    {
        std::string msg = std::vformat(fmt, std::make_format_args(args...));
        if (msg.empty())
        {
            return;
        }

        // One write for the line, or for the box it belongs to, when the outermost block ends.
        CtLogBlock block;
        coretrace::write_log_line(level, {}, msg, std::source_location::current());
    }
    catch (...)
    {
        static constexpr char fallback[] = "ct: log format error\n";
        CtLogBlock line;
        coretrace::write_raw(fallback, sizeof(fallback) - 1);
    }
}

// An allocation, as both runtimes log it: `label` and the site, then a box with `status`
// ("reachable", "unreachable"), the requested and the allocated sizes, and the address.
#if defined(_WIN32)
// The leak report of the Windows runtime, which runs at process exit. False when it was
// skipped, another thread holding the allocation table (#187).
CT_NOINSTR bool ct_report_leaks(void);
#endif

CT_NOINSTR void ct_log_alloc_details(const char* label, const char* status, size_t req_size,
                                     size_t real_size, void* ptr, const char* site, CTColor color,
                                     CTLevel lvl);

// The other allocation events, in the words both runtimes use. `label` names the API, such
// as tracing-free, tracing-delete-array or tracing-munmap; `kind` is the name of the kind of
// allocation the runtime recorded, such as malloc, new[] or mmap.
// A resize: `status` is allocated, in-place, moved, updated, freed or failed.
CT_NOINSTR void ct_log_realloc_details(const char* label, const char* status, size_t old_req_size,
                                       size_t old_real_size, void* old_ptr, size_t new_req_size,
                                       size_t new_real_size, void* new_ptr, const char* site,
                                       CTColor color);
// A resize of a block already released.
CT_NOINSTR void ct_log_realloc_freed(void* ptr, const char* site);
// The release of a tracked block, with the kind and the site of its allocation.
CT_NOINSTR void ct_log_release(const char* label, void* ptr, size_t size, const char* kind,
                               const char* alloc_site);
// The release of a block the runtime did not track, such as one a library allocated.
CT_NOINSTR void ct_log_release_unknown(const char* label, void* ptr);
CT_NOINSTR void ct_log_release_null(const char* label);
// A second release of a block: `site` is where it happens.
CT_NOINSTR void ct_log_double_free(const char* label, void* ptr, const char* site,
                                   const char* alloc_site);
// A release through an API that does not match the allocation's, such as delete on malloc.
CT_NOINSTR void ct_log_deallocator_mismatch(const char* label, void* ptr, const char* expected,
                                            const char* recorded, const char* alloc_site);
CT_NOINSTR void ct_log_autofree(void* ptr, size_t size, const char* site);
// An auto-free that releases nothing: a null pointer, or `reason`.
CT_NOINSTR void ct_log_autofree_skipped(void* ptr, const char* reason);

#endif // CT_RUNTIME_INTERNAL_H
