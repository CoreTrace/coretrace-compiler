// SPDX-License-Identifier: Apache-2.0
//
// Conservative auto-free scan: decides whether an allocation the compiler proved
// unused is still reachable from the mutator's roots (thread registers, thread
// stacks and the images' __DATA segments) before the runtime releases it. The
// periodic scan also releases every block it cannot reach from those roots,
// directly or through other tracked blocks.
//
// The scan suspends every other thread while it reads their registers and stacks,
// so it only exists where that is possible; elsewhere the entry points below are
// no-ops and every auto-free decision rests on the compile-time analysis alone.
#include "ct_runtime_alloc_internal.h"
#include "ct_runtime_quarantine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <new>
#include <sys/mman.h>
#include <time.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <malloc/malloc.h>
#include <pthread.h>
#endif

static std::atomic<int> ct_autofree_scan_initialized{0};
static std::atomic<int> ct_autofree_scan_enabled{0};
static std::atomic<int> ct_autofree_scan_start{0};
static std::atomic<int> ct_autofree_scan_in_progress{0};
static std::atomic<int> ct_autofree_scan_stack{1};
static std::atomic<int> ct_autofree_scan_regs{1};
static std::atomic<int> ct_autofree_scan_globals{1};
static std::atomic<int> ct_autofree_scan_interior{1};
static std::atomic<int> ct_autofree_scan_debug{0};
static std::atomic<int> ct_autofree_scan_ptr{1};
static std::atomic<uint64_t> ct_autofree_scan_interval_ns{0};
static std::atomic<uint64_t> ct_autofree_scan_period_ns{0};
static std::atomic<uint64_t> ct_autofree_scan_budget_ns{0};
static std::atomic<uint64_t> ct_autofree_scan_last_ns{0};
static std::atomic<uint64_t> ct_autofree_scan_last_gc_ns{0};
static pthread_t ct_autofree_scan_thread;
static std::atomic<int> ct_autofree_scan_thread_started{0};
#if defined(__APPLE__)
// The periodic scan's own thread, which the scan skips: it holds none of the program's
// references, only stale copies of the words earlier passes read.
static std::atomic<thread_t> ct_autofree_scan_thread_port{MACH_PORT_NULL};
#endif
static size_t ct_autofree_scan_timeout_check_freq = 100;
CT_NODISCARD CT_NOINSTR static uint64_t ct_time_ns(void)
{
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

CT_NODISCARD CT_NOINSTR static uint64_t ct_env_u64(const char* name, uint64_t def_value)
{
    const char* value = std::getenv(name);
    if (!value || !*value)
    {
        return def_value;
    }
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value)
    {
        return def_value;
    }
    return static_cast<uint64_t>(parsed);
}

CT_NODISCARD CT_NOINSTR static double ct_env_f64(const char* name, double def_value)
{
    const char* value = std::getenv(name);
    if (!value || !*value)
    {
        return def_value;
    }
    char* end = nullptr;
    errno = 0;
    double parsed = std::strtod(value, &end);
    if (end == value || errno != 0)
    {
        return def_value;
    }
    return parsed;
}

CT_NODISCARD CT_NOINSTR static int ct_env_flag(const char* name, int def_value)
{
    const char* value = std::getenv(name);
    if (!value)
    {
        return def_value;
    }
    if (*value == '\0')
    {
        return def_value;
    }
    if (*value == '0')
    {
        return 0;
    }
    return 1;
}

CT_NOINSTR void ct_autofree_scan_init_once(void)
{
    int expected = 0;
    if (!ct_autofree_scan_initialized.compare_exchange_strong(expected, 1,
                                                              std::memory_order_acq_rel))
    {
        return;
    }

    const int scan_enabled = ct_env_flag("CT_AUTOFREE_SCAN", 0);
    const int scan_start = ct_env_flag("CT_AUTOFREE_SCAN_START", 0);
    const int scan_stack = ct_env_flag("CT_AUTOFREE_SCAN_STACK", 1);
    const int scan_regs = ct_env_flag("CT_AUTOFREE_SCAN_REGS", 1);
    const int scan_globals = ct_env_flag("CT_AUTOFREE_SCAN_GLOBALS", 1);
    const int scan_interior = ct_env_flag("CT_AUTOFREE_SCAN_INTERIOR", 1);
    const int scan_debug = static_cast<int>(ct_env_u64("CT_DEBUG_AUTOFREE_SCAN", 0));
    const int scan_ptr = ct_env_flag("CT_AUTOFREE_SCAN_PTR", 1);
    const uint64_t interval_ns = ct_env_u64("CT_AUTOFREE_SCAN_INTERVAL_MS", 0) * 1000000ULL;

    const uint64_t period_ns = ct_env_u64("CT_AUTOFREE_SCAN_PERIOD_NS", 0);
    const uint64_t period_us = ct_env_u64("CT_AUTOFREE_SCAN_PERIOD_US", 0);
    const double period_ms = ct_env_f64("CT_AUTOFREE_SCAN_PERIOD_MS", 0.0);

    uint64_t final_period_ns = 0;
    if (period_ns)
    {
        final_period_ns = period_ns;
    }
    else if (period_us)
    {
        final_period_ns = period_us * 1000ULL;
    }
    else if (period_ms > 0.0)
    {
        final_period_ns = static_cast<uint64_t>(period_ms * 1000000.0);
    }

    const uint64_t budget_ns = ct_env_u64("CT_AUTOFREE_SCAN_BUDGET_NS", 0);
    const uint64_t budget_us = ct_env_u64("CT_AUTOFREE_SCAN_BUDGET_US", 0);
    const double budget_ms = ct_env_f64("CT_AUTOFREE_SCAN_BUDGET_MS", 5.0);

    uint64_t final_budget_ns = 0;
    if (budget_ns)
    {
        final_budget_ns = budget_ns;
    }
    else if (budget_us)
    {
        final_budget_ns = budget_us * 1000ULL;
    }
    else if (budget_ms > 0.0)
    {
        final_budget_ns = static_cast<uint64_t>(budget_ms * 1000000.0);
    }

    int final_scan_enabled = scan_enabled;
    if (scan_start)
    {
        final_scan_enabled = 1;
        if (final_period_ns == 0)
        {
            final_period_ns = 1000000000ULL;
        }
    }

    ct_autofree_scan_enabled.store(final_scan_enabled, std::memory_order_release);
    ct_autofree_scan_start.store(scan_start, std::memory_order_release);
    ct_autofree_scan_stack.store(scan_stack, std::memory_order_release);
    ct_autofree_scan_regs.store(scan_regs, std::memory_order_release);
    ct_autofree_scan_globals.store(scan_globals, std::memory_order_release);
    ct_autofree_scan_interior.store(scan_interior, std::memory_order_release);
    ct_autofree_scan_debug.store(scan_debug, std::memory_order_release);
    ct_autofree_scan_ptr.store(scan_ptr, std::memory_order_release);
    ct_autofree_scan_interval_ns.store(interval_ns, std::memory_order_release);
    ct_autofree_scan_period_ns.store(final_period_ns, std::memory_order_release);
    ct_autofree_scan_budget_ns.store(final_budget_ns, std::memory_order_release);
}

CT_NODISCARD CT_NOINSTR static int ct_autofree_scan_should_run(void)
{
    if (!ct_autofree_scan_enabled.load(std::memory_order_acquire))
    {
        return 0;
    }
    if (ct_autofree_scan_in_progress.load(std::memory_order_acquire))
    {
        return 0;
    }
    uint64_t now = ct_time_ns();
    uint64_t interval_ns = ct_autofree_scan_interval_ns.load(std::memory_order_relaxed);
    uint64_t last_ns = ct_autofree_scan_last_ns.load(std::memory_order_relaxed);
    if (interval_ns != 0 && now - last_ns < interval_ns)
    {
        return 0;
    }
    ct_autofree_scan_last_ns.store(now, std::memory_order_relaxed);
    return 1;
}

CT_NODISCARD CT_NOINSTR static int ct_scan_time_exceeded(uint64_t start_ns)
{
    if (ct_autofree_scan_budget_ns.load(std::memory_order_relaxed) == 0)
    {
        return 0;
    }
    return (ct_time_ns() - start_ns) >= ct_autofree_scan_budget_ns.load(std::memory_order_relaxed);
}

CT_NODISCARD CT_NOINSTR static int ct_scan_time_exceeded_fast(uint64_t start_ns,
                                                              size_t* check_counter)
{
    if (ct_autofree_scan_budget_ns.load(std::memory_order_relaxed) == 0)
    {
        return 0;
    }
    if (++(*check_counter) >= ct_autofree_scan_timeout_check_freq)
    {
        *check_counter = 0;
        return (ct_time_ns() - start_ns) >=
               ct_autofree_scan_budget_ns.load(std::memory_order_relaxed);
    }
    return 0;
}

// A tracked block's address range. The periodic scan lists the live blocks' ranges,
// sorted, to look up each word it reads by binary search.
struct ct_block_range
{
    uintptr_t begin;
    uintptr_t end;
    struct ct_alloc_entry* entry;
};

// The periodic scan's work buffers, grown before the threads are suspended and kept across
// passes: the live blocks' ranges, and the marked blocks whose contents are still to be
// scanned, each pushed once. Only the scan that owns ct_autofree_scan_guard uses them.
static struct ct_block_range* ct_scan_ranges = nullptr;
static size_t ct_scan_range_count = 0;
static size_t ct_scan_ranges_capacity = 0;
static struct ct_alloc_entry** ct_scan_pending = nullptr;
static size_t ct_scan_pending_count = 0;
static size_t ct_scan_pending_capacity = 0;

// The live block `value` points to, or into when interior pointers count; nullptr if none.
CT_NODISCARD CT_NOINSTR static struct ct_alloc_entry* ct_scan_find_block(uintptr_t value)
{
    if (ct_scan_range_count == 0 || value < ct_scan_ranges[0].begin ||
        value > ct_scan_ranges[ct_scan_range_count - 1].end)
    {
        return nullptr;
    }
    const struct ct_block_range* after =
        std::upper_bound(ct_scan_ranges, ct_scan_ranges + ct_scan_range_count, value,
                         [](uintptr_t address, const struct ct_block_range& range)
                         { return address < range.begin; });
    const struct ct_block_range& range = after[-1];
    if (value == range.begin ||
        (value < range.end && ct_autofree_scan_interior.load(std::memory_order_relaxed)))
    {
        return range.entry;
    }
    return nullptr;
}

CT_NOINSTR static void ct_autofree_mark_value(uintptr_t value)
{
    struct ct_alloc_entry* entry = ct_scan_find_block(value);
    if (entry && !entry->mark)
    {
        entry->mark = 1;
        ct_scan_pending[ct_scan_pending_count++] = entry;
    }
}

CT_NODISCARD CT_NOINSTR static int ct_autofree_gc_should_run(void)
{
    if (!ct_autofree_scan_enabled.load(std::memory_order_acquire) ||
        !ct_autofree_scan_start.load(std::memory_order_acquire))
    {
        return 0;
    }
    if (ct_autofree_scan_period_ns.load(std::memory_order_relaxed) == 0)
    {
        return 1;
    }
    uint64_t now = ct_time_ns();
    uint64_t period_ns = ct_autofree_scan_period_ns.load(std::memory_order_relaxed);
    uint64_t last_gc = ct_autofree_scan_last_gc_ns.load(std::memory_order_relaxed);
    if (now - last_gc < period_ns)
    {
        return 0;
    }
    ct_autofree_scan_last_gc_ns.store(now, std::memory_order_relaxed);
    return 1;
}

CT_NODISCARD CT_NOINSTR static int ct_ptr_in_range(uintptr_t value, uintptr_t base, size_t size)
{
    if (!size)
    {
        return 0;
    }
    uintptr_t end = base + size;
    if (end < base)
    {
        end = static_cast<uintptr_t>(-1);
    }
    return value >= base && value < end;
}

CT_NODISCARD CT_NOINSTR static int ct_scan_range_for_ptr(uintptr_t base, size_t size,
                                                         const void* begin, const void* end,
                                                         uint64_t start_ns)
{
    uintptr_t start = reinterpret_cast<uintptr_t>(begin);
    uintptr_t finish = reinterpret_cast<uintptr_t>(end);
    if (finish <= start)
    {
        return 0;
    }

    uintptr_t align_mask = sizeof(uintptr_t) - 1;
    start = (start + align_mask) & ~align_mask;
    finish = finish & ~align_mask;
    if (finish <= start)
    {
        return 0;
    }

    const uintptr_t* cursor = reinterpret_cast<const uintptr_t*>(start);
    const uintptr_t* end_ptr = reinterpret_cast<const uintptr_t*>(finish);
    size_t counter = 0;
    while (cursor < end_ptr)
    {
        uintptr_t value = *cursor;
        if (ct_ptr_in_range(value, base, size))
        {
            return 1;
        }
        ++cursor;
        if (ct_scan_time_exceeded_fast(start_ns, &counter))
        {
            return 1;
        }
    }
    return 0;
}

CT_NOINSTR static void ct_scan_range_for_marks(const void* begin, const void* end,
                                               uint64_t start_ns, int* timed_out)
{
    if (timed_out && *timed_out)
    {
        return;
    }
    uintptr_t start = reinterpret_cast<uintptr_t>(begin);
    uintptr_t finish = reinterpret_cast<uintptr_t>(end);
    if (finish <= start)
    {
        return;
    }

    uintptr_t align_mask = sizeof(uintptr_t) - 1;
    start = (start + align_mask) & ~align_mask;
    finish = finish & ~align_mask;
    if (finish <= start)
    {
        return;
    }

    const uintptr_t* cursor = reinterpret_cast<const uintptr_t*>(start);
    const uintptr_t* end_ptr = reinterpret_cast<const uintptr_t*>(finish);
    size_t counter = 0;
    while (cursor < end_ptr)
    {
        ct_autofree_mark_value(*cursor);
        ++cursor;
        if (ct_scan_time_exceeded_fast(start_ns, &counter))
        {
            if (timed_out)
            {
                *timed_out = 1;
            }
            return;
        }
    }
}

#if defined(__APPLE__)
CT_NODISCARD CT_NOINSTR static int ct_thread_get_sp(thread_t thread, uintptr_t* sp_out)
{
#if defined(__aarch64__) || defined(__arm64__)
    arm_thread_state64_t state;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state),
                         &count) != KERN_SUCCESS)
    {
        return 0;
    }
    if (sp_out)
    {
        *sp_out = static_cast<uintptr_t>(state.__sp);
    }
    return 1;
#elif defined(__x86_64__)
    x86_thread_state64_t state;
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, x86_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state),
                         &count) != KERN_SUCCESS)
    {
        return 0;
    }
    if (sp_out)
    {
        *sp_out = static_cast<uintptr_t>(state.__rsp);
    }
    return 1;
#else
    (void)thread;
    (void)sp_out;
    return 0;
#endif
}

CT_NODISCARD CT_NOINSTR static int ct_scan_regs_for_ptr(thread_t thread, uintptr_t base,
                                                        size_t size, uint64_t start_ns)
{
#if defined(__aarch64__) || defined(__arm64__)
    arm_thread_state64_t state;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state),
                         &count) != KERN_SUCCESS)
    {
        return 0;
    }
    for (int i = 0; i < 29; ++i)
    {
        if (ct_ptr_in_range(static_cast<uintptr_t>(state.__x[i]), base, size))
        {
            return 1;
        }
    }
    if (ct_ptr_in_range(static_cast<uintptr_t>(state.__fp), base, size))
    {
        return 1;
    }
    if (ct_ptr_in_range(static_cast<uintptr_t>(state.__lr), base, size))
    {
        return 1;
    }
    if (ct_ptr_in_range(static_cast<uintptr_t>(state.__sp), base, size))
    {
        return 1;
    }
    if (ct_ptr_in_range(static_cast<uintptr_t>(state.__pc), base, size))
    {
        return 1;
    }
    return ct_scan_time_exceeded(start_ns) ? 1 : 0;
#elif defined(__x86_64__)
    x86_thread_state64_t state;
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, x86_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state),
                         &count) != KERN_SUCCESS)
    {
        return 0;
    }
    const uintptr_t regs[] = {
        static_cast<uintptr_t>(state.__rax), static_cast<uintptr_t>(state.__rbx),
        static_cast<uintptr_t>(state.__rcx), static_cast<uintptr_t>(state.__rdx),
        static_cast<uintptr_t>(state.__rdi), static_cast<uintptr_t>(state.__rsi),
        static_cast<uintptr_t>(state.__rbp), static_cast<uintptr_t>(state.__rsp),
        static_cast<uintptr_t>(state.__r8),  static_cast<uintptr_t>(state.__r9),
        static_cast<uintptr_t>(state.__r10), static_cast<uintptr_t>(state.__r11),
        static_cast<uintptr_t>(state.__r12), static_cast<uintptr_t>(state.__r13),
        static_cast<uintptr_t>(state.__r14), static_cast<uintptr_t>(state.__r15),
        static_cast<uintptr_t>(state.__rip),
    };
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i)
    {
        if (ct_ptr_in_range(regs[i], base, size))
        {
            return 1;
        }
    }
    return ct_scan_time_exceeded(start_ns) ? 1 : 0;
#else
    (void)thread;
    (void)base;
    (void)size;
    return ct_scan_time_exceeded(start_ns) ? 1 : 0;
#endif
}

CT_NOINSTR static void ct_scan_regs_for_marks(thread_t thread, uint64_t start_ns, int* timed_out)
{
#if defined(__aarch64__) || defined(__arm64__)
    arm_thread_state64_t state;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state),
                         &count) != KERN_SUCCESS)
    {
        return;
    }
    for (int i = 0; i < 29; ++i)
    {
        ct_autofree_mark_value(static_cast<uintptr_t>(state.__x[i]));
    }
    ct_autofree_mark_value(static_cast<uintptr_t>(state.__fp));
    ct_autofree_mark_value(static_cast<uintptr_t>(state.__lr));
    ct_autofree_mark_value(static_cast<uintptr_t>(state.__sp));
    ct_autofree_mark_value(static_cast<uintptr_t>(state.__pc));
    if (ct_scan_time_exceeded(start_ns) && timed_out)
    {
        *timed_out = 1;
    }
#elif defined(__x86_64__)
    x86_thread_state64_t state;
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, x86_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state),
                         &count) != KERN_SUCCESS)
    {
        return;
    }
    const uintptr_t regs[] = {
        static_cast<uintptr_t>(state.__rax), static_cast<uintptr_t>(state.__rbx),
        static_cast<uintptr_t>(state.__rcx), static_cast<uintptr_t>(state.__rdx),
        static_cast<uintptr_t>(state.__rdi), static_cast<uintptr_t>(state.__rsi),
        static_cast<uintptr_t>(state.__rbp), static_cast<uintptr_t>(state.__rsp),
        static_cast<uintptr_t>(state.__r8),  static_cast<uintptr_t>(state.__r9),
        static_cast<uintptr_t>(state.__r10), static_cast<uintptr_t>(state.__r11),
        static_cast<uintptr_t>(state.__r12), static_cast<uintptr_t>(state.__r13),
        static_cast<uintptr_t>(state.__r14), static_cast<uintptr_t>(state.__r15),
        static_cast<uintptr_t>(state.__rip),
    };
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i)
    {
        ct_autofree_mark_value(regs[i]);
    }
    if (ct_scan_time_exceeded(start_ns) && timed_out)
    {
        *timed_out = 1;
    }
#else
    (void)thread;
    (void)start_ns;
    if (timed_out)
    {
        *timed_out = 1;
    }
#endif
}

// A thread of the process, as a scan sees it. Its stack bounds are read before any thread
// is suspended: pthread_from_mach_thread_np takes a lock a suspended thread may hold. Only a
// thread the scan could suspend, or the scanning thread, is read: one that could not be
// suspended has exited, and its stack may already be gone.
struct ct_scan_thread
{
    thread_t port;
    uintptr_t stack_low; // above the guard page
    uintptr_t stack_high;
    bool readable;
};

struct ct_scan_threads
{
    struct ct_scan_thread* items;
    mach_msg_type_number_t count;
    thread_t self;
};

// Lists the process's threads and their stacks, before any is suspended. False when they
// cannot be listed, with nothing to release.
CT_NODISCARD CT_NOINSTR static bool ct_list_threads(struct ct_scan_threads* threads)
{
    thread_act_array_t ports = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &ports, &count) != KERN_SUCCESS)
    {
        return false;
    }
    threads->items =
        static_cast<struct ct_scan_thread*>(std::malloc(count * sizeof(struct ct_scan_thread)));
    if (threads->items)
    {
        for (mach_msg_type_number_t i = 0; i < count; ++i)
        {
            struct ct_scan_thread& thread = threads->items[i];
            thread = {ports[i], 0, 0, false};
            const pthread_t pthread = pthread_from_mach_thread_np(ports[i]);
            if (!pthread)
            {
                continue;
            }
            const uintptr_t high = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread));
            const size_t size = pthread_get_stacksize_np(pthread);
            if (!high || !size)
            {
                continue;
            }
            thread.stack_low = high - size;
            if (thread.stack_low + vm_page_size < high)
            {
                thread.stack_low += vm_page_size;
            }
            thread.stack_high = high;
        }
    }
    else
    {
        for (mach_msg_type_number_t i = 0; i < count; ++i)
        {
            mach_port_deallocate(mach_task_self(), ports[i]);
        }
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(ports),
                  count * sizeof(thread_t));
    if (!threads->items)
    {
        return false;
    }
    threads->count = count;
    threads->self = mach_thread_self();
    return true;
}

// Suspends every listed thread but the calling one.
CT_NOINSTR static void ct_suspend_threads(struct ct_scan_threads* threads)
{
    for (mach_msg_type_number_t i = 0; i < threads->count; ++i)
    {
        struct ct_scan_thread& thread = threads->items[i];
        thread.readable =
            thread.port == threads->self || thread_suspend(thread.port) == KERN_SUCCESS;
    }
}

// Resumes the threads ct_suspend_threads suspended, and releases the list.
CT_NOINSTR static void ct_resume_threads(struct ct_scan_threads* threads)
{
    for (mach_msg_type_number_t i = 0; i < threads->count; ++i)
    {
        const struct ct_scan_thread& thread = threads->items[i];
        if (thread.readable && thread.port != threads->self)
        {
            thread_resume(thread.port);
        }
        mach_port_deallocate(mach_task_self(), thread.port);
    }
    mach_port_deallocate(mach_task_self(), threads->self);
    std::free(threads->items);
}

// The part of a thread's stack in use, from its stack pointer, or the whole stack when that
// cannot be read, to its top. False when the thread's stack is unknown.
CT_NODISCARD CT_NOINSTR static bool ct_thread_stack_in_use(const struct ct_scan_thread& thread,
                                                           const void** begin, const void** end)
{
    if (!thread.stack_high)
    {
        return false;
    }
    uintptr_t sp = 0;
    const bool sp_known =
        ct_thread_get_sp(thread.port, &sp) && sp >= thread.stack_low && sp < thread.stack_high;
    *begin = reinterpret_cast<const void*>(sp_known ? sp : thread.stack_low);
    *end = reinterpret_cast<const void*>(thread.stack_high);
    return true;
}

CT_NODISCARD CT_NOINSTR static int ct_scan_thread_stack_for_ptr(const struct ct_scan_thread& thread,
                                                                uintptr_t base, size_t size,
                                                                uint64_t start_ns)
{
    const void* begin = nullptr;
    const void* end = nullptr;
    return ct_thread_stack_in_use(thread, &begin, &end) &&
           ct_scan_range_for_ptr(base, size, begin, end, start_ns);
}

CT_NOINSTR static void ct_scan_thread_stack_for_marks(const struct ct_scan_thread& thread,
                                                      uint64_t start_ns, int* timed_out)
{
    const void* begin = nullptr;
    const void* end = nullptr;
    if (ct_thread_stack_in_use(thread, &begin, &end))
    {
        ct_scan_range_for_marks(begin, end, start_ns, timed_out);
    }
}

// Optimized function: scan regs and stack together in one pass
CT_NOINSTR static void ct_scan_thread_regs_and_stack_marks(const struct ct_scan_thread& thread,
                                                           uint64_t start_ns, int* timed_out)
{
    if (timed_out && *timed_out)
    {
        return;
    }

    // Scan regs if enabled
    if (ct_autofree_scan_regs.load(std::memory_order_relaxed))
    {
        ct_scan_regs_for_marks(thread.port, start_ns, timed_out);
    }

    // Then scan stack if enabled and not timed out
    if (ct_autofree_scan_stack.load(std::memory_order_relaxed) && (!timed_out || !*timed_out))
    {
        ct_scan_thread_stack_for_marks(thread, start_ns, timed_out);
    }
}

// Claims ct_autofree_scan_in_progress for the lifetime of a scan and releases it on every
// exit path, so an early return can never leave the flag set and starve later scans. Two
// scans at once would suspend each other's threads, and each other.
struct ct_autofree_scan_guard
{
    int owned;

    CT_NOINSTR ct_autofree_scan_guard()
        : owned(!ct_autofree_scan_in_progress.exchange(1, std::memory_order_acq_rel))
    {
    }

    CT_NOINSTR ~ct_autofree_scan_guard()
    {
        if (owned)
        {
            ct_autofree_scan_in_progress.store(0, std::memory_order_release);
        }
    }

    ct_autofree_scan_guard(const ct_autofree_scan_guard&) = delete;
    ct_autofree_scan_guard& operator=(const ct_autofree_scan_guard&) = delete;
};

// The images' data segments, the global roots of a scan. dyld's functions may take a lock
// a suspended thread holds, so they are listed before the threads are suspended (#118).
// An image loaded or unloaded since then changes ct_image_generation, which the scan checks
// while the threads are suspended: a list older than that cannot be relied on. An image
// still being loaded has run no initializer, so its data holds no heap pointer yet; one
// being unloaded is reported before its memory goes.
struct ct_data_range
{
    uintptr_t begin;
    uintptr_t end;
};

static struct ct_data_range* ct_data_ranges = nullptr;
static size_t ct_data_range_count = 0;
static size_t ct_data_range_capacity = 0;
static std::atomic<uint64_t> ct_image_generation{0};

CT_NOINSTR static void ct_on_image_change(const struct mach_header*, intptr_t)
{
    ct_image_generation.fetch_add(1, std::memory_order_acq_rel);
}

// Grows *buffer to hold `needed` items of `item_size` bytes. Only outside suspension.
CT_NODISCARD CT_NOINSTR static bool ct_reserve(void** buffer, size_t* capacity, size_t needed,
                                               size_t item_size)
{
    if (needed <= *capacity)
    {
        return true;
    }
    size_t grown = *capacity ? *capacity : 64;
    while (grown < needed)
    {
        grown *= 2;
    }
    void* resized = std::realloc(*buffer, grown * item_size);
    if (!resized)
    {
        return false;
    }
    *buffer = resized;
    *capacity = grown;
    return true;
}

// Appends [begin, end) to ct_data_ranges, less the allocation table's static storage.
CT_NODISCARD CT_NOINSTR static bool ct_add_data_range(size_t* count, uintptr_t begin, uintptr_t end)
{
    uintptr_t table_begin = 0;
    uintptr_t table_end = 0;
    ct_alloc_table_storage_bounds(&table_begin, &table_end);
    const struct ct_data_range pieces[] = {
        {begin, std::min(end, table_begin)},
        {std::max(begin, table_end), end},
    };
    for (const struct ct_data_range& piece : pieces)
    {
        if (piece.begin >= piece.end)
        {
            continue;
        }
        void* buffer = ct_data_ranges;
        if (!ct_reserve(&buffer, &ct_data_range_capacity, *count + 1, sizeof(struct ct_data_range)))
        {
            return false;
        }
        ct_data_ranges = static_cast<struct ct_data_range*>(buffer);
        ct_data_ranges[(*count)++] = piece;
    }
    return true;
}

// Lists the data segments into ct_data_ranges and returns, in *generation, the image
// generation they belong to. Needs the scan guard; only outside suspension.
CT_NODISCARD CT_NOINSTR static bool ct_list_data_segments(uint64_t* generation)
{
    static std::atomic<int> watching{0};
    int expected = 0;
    if (watching.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
    {
        _dyld_register_func_for_add_image(ct_on_image_change);
        _dyld_register_func_for_remove_image(ct_on_image_change);
    }
    // Read first: an image added while listing then shows as a changed generation.
    *generation = ct_image_generation.load(std::memory_order_acquire);

    size_t count = 0;
    const uint32_t image_count = _dyld_image_count();
    for (uint32_t i = 0; i < image_count; ++i)
    {
        const mach_header_64* header =
            reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
        if (!header || header->magic != MH_MAGIC_64)
        {
            continue;
        }
        const intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        const load_command* cmd = reinterpret_cast<const load_command*>(
            reinterpret_cast<const char*>(header) + sizeof(mach_header_64));
        for (uint32_t c = 0; c < header->ncmds; ++c)
        {
            if (cmd->cmd == LC_SEGMENT_64)
            {
                const segment_command_64* seg = reinterpret_cast<const segment_command_64*>(cmd);
                if (std::strncmp(seg->segname, "__DATA", 6) == 0)
                {
                    const uintptr_t begin = static_cast<uintptr_t>(seg->vmaddr + slide);
                    if (!ct_add_data_range(&count, begin,
                                           begin + static_cast<uintptr_t>(seg->vmsize)))
                    {
                        return false;
                    }
                }
            }
            cmd = reinterpret_cast<const load_command*>(reinterpret_cast<const char*>(cmd) +
                                                        cmd->cmdsize);
        }
    }
    ct_data_range_count = count;
    return true;
}

// Whether the listed data segments still describe the loaded images.
CT_NODISCARD CT_NOINSTR static bool ct_data_segments_current(uint64_t generation)
{
    return ct_image_generation.load(std::memory_order_acquire) == generation;
}

CT_NODISCARD CT_NOINSTR static int ct_scan_globals_for_ptr(uintptr_t base, size_t size,
                                                           uint64_t start_ns)
{
    for (size_t i = 0; i < ct_data_range_count; ++i)
    {
        if (ct_scan_range_for_ptr(base, size, reinterpret_cast<void*>(ct_data_ranges[i].begin),
                                  reinterpret_cast<void*>(ct_data_ranges[i].end), start_ns))
        {
            return 1;
        }
    }
    return 0;
}

CT_NOINSTR static void ct_scan_globals_for_marks(uint64_t start_ns, int* timed_out)
{
    for (size_t i = 0; i < ct_data_range_count && !*timed_out; ++i)
    {
        ct_scan_range_for_marks(reinterpret_cast<void*>(ct_data_ranges[i].begin),
                                reinterpret_cast<void*>(ct_data_ranges[i].end), start_ns,
                                timed_out);
    }
}

CT_NODISCARD CT_NOINSTR int ct_autofree_scan_for_ptr(void* ptr, size_t size)
{
    if (!ct_autofree_scan_should_run())
    {
        return 0;
    }

    ct_autofree_scan_guard scan_guard;
    if (!scan_guard.owned)
    {
        return 0;
    }
    const bool scan_globals = ct_autofree_scan_globals.load(std::memory_order_relaxed);
    uint64_t generation = 0;
    if (scan_globals && !ct_list_data_segments(&generation))
    {
        return 1;
    }

    uint64_t start_ns = ct_time_ns();
    uintptr_t base = reinterpret_cast<uintptr_t>(ptr);

    struct ct_scan_threads threads = {};
    if (!ct_list_threads(&threads))
    {
        return 0;
    }
    ct_suspend_threads(&threads);

    int found = 0;
    for (mach_msg_type_number_t i = 0; i < threads.count && !found; ++i)
    {
        const struct ct_scan_thread& thread = threads.items[i];
        if (!thread.readable)
        {
            continue;
        }
        if (ct_autofree_scan_regs.load(std::memory_order_relaxed))
        {
            if (ct_scan_regs_for_ptr(thread.port, base, size, start_ns))
            {
                found = 1;
                break;
            }
        }
        if (ct_autofree_scan_stack.load(std::memory_order_relaxed))
        {
            if (ct_scan_thread_stack_for_ptr(thread, base, size, start_ns))
            {
                found = 1;
                break;
            }
        }
        if (ct_scan_time_exceeded(start_ns))
        {
            found = 1;
            break;
        }
    }

    if (!found && scan_globals)
    {
        // Images changed since the list was made: a reference may be where it cannot see.
        if (!ct_data_segments_current(generation) || ct_scan_globals_for_ptr(base, size, start_ns))
        {
            found = 1;
        }
    }

    ct_resume_threads(&threads);

    const int debug_level = ct_autofree_scan_debug.load(std::memory_order_relaxed);
    if (debug_level > 1 || (debug_level == 1 && found))
    {
        ct_log(CTLevel::Warn, "{}ct: autofree scan {} for ptr={:p} size={}{}\n",
               ct_color(CTColor::BgBrightYellow), found ? "found" : "clear", ptr, size,
               ct_color(CTColor::Reset));
    }
    return found;
}

CT_NOINSTR static const char* ct_alloc_kind_label(unsigned char kind)
{
    switch (kind)
    {
    case CT_ALLOC_KIND_MALLOC:
        return "malloc";
    case CT_ALLOC_KIND_NEW:
        return "new";
    case CT_ALLOC_KIND_NEW_ARRAY:
        return "new[]";
    case CT_ALLOC_KIND_MMAP:
        return "mmap";
    case CT_ALLOC_KIND_SBRK:
        return "sbrk";
    default:
        return "unknown";
    }
}

CT_NOINSTR static void ct_autofree_do_free(const struct ct_autofree_free_item& item)
{
    if (!item.ptr)
    {
        return;
    }
    switch (item.kind)
    {
    case CT_ALLOC_KIND_NEW:
        if (ct_is_enabled(CT_FEATURE_SHADOW))
        {
            ct_shadow_poison_range(item.ptr, item.size);
        }
        ct_log(CTLevel::Warn, "{}auto-free(scan) kind={} ptr={:p} size={} site={}{}\n",
               ct_color(CTColor::BgBrightYellow), ct_alloc_kind_label(item.kind), item.ptr,
               item.size, ct_site_name(item.site), ct_color(CTColor::Reset));
        ct_quarantine_push(
            {item.ptr, item.size, static_cast<unsigned char>(CtReleaseApi::Delete), item.kind});
        break;
    case CT_ALLOC_KIND_NEW_ARRAY:
        if (ct_is_enabled(CT_FEATURE_SHADOW))
        {
            ct_shadow_poison_range(item.ptr, item.size);
        }
        ct_log(CTLevel::Warn, "{}auto-free(scan) kind={} ptr={:p} size={} site={}{}\n",
               ct_color(CTColor::BgBrightYellow), ct_alloc_kind_label(item.kind), item.ptr,
               item.size, ct_site_name(item.site), ct_color(CTColor::Reset));
        ct_quarantine_push({item.ptr, item.size,
                            static_cast<unsigned char>(CtReleaseApi::DeleteArray), item.kind});
        break;
    case CT_ALLOC_KIND_MMAP:
        ct_log(CTLevel::Warn, "{}auto-free(scan) kind={} ptr={:p} size={} site={}{}\n",
               ct_color(CTColor::BgBrightYellow), ct_alloc_kind_label(item.kind), item.ptr,
               item.size, ct_site_name(item.site), ct_color(CTColor::Reset));
        ct_forget_returned_block(item.ptr, item.size);
        (void)munmap(item.ptr, item.size);
        break;
    case CT_ALLOC_KIND_SBRK:
#if defined(__linux__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    {
        void* current = sbrk(0);
        if (current != (void*)-1 &&
            static_cast<char*>(item.ptr) + static_cast<ptrdiff_t>(item.size) == current)
        {
            (void)sbrk(-static_cast<intptr_t>(item.size));
            ct_forget_returned_block(item.ptr, item.size);
            ct_log(CTLevel::Warn, "{}auto-free(scan) kind={} ptr={:p} size={} site={}{}\n",
                   ct_color(CTColor::BgBrightYellow), ct_alloc_kind_label(item.kind), item.ptr,
                   item.size, ct_site_name(item.site), ct_color(CTColor::Reset));
            break;
        }
        ct_log(CTLevel::Warn, "{}ct: auto-free skipped ptr={:p} (sbrk not top){}\n",
               ct_color(CTColor::BgBrightYellow), item.ptr, ct_color(CTColor::Reset));
        break;
    }
#pragma clang diagnostic pop
#else
        ct_log(CTLevel::Warn, "{}ct: auto-free skipped ptr={:p} (sbrk not supported){}\n",
               ct_color(CTColor::BgBrightYellow), item.ptr, ct_color(CTColor::Reset));
        break;
#endif
    case CT_ALLOC_KIND_MALLOC:
    default:
        if (ct_is_enabled(CT_FEATURE_SHADOW))
        {
            ct_shadow_poison_range(item.ptr, item.size);
        }
        ct_log(CTLevel::Warn, "{}auto-free(scan) kind={} ptr={:p} size={} site={}{}\n",
               ct_color(CTColor::BgBrightYellow), ct_alloc_kind_label(item.kind), item.ptr,
               item.size, ct_site_name(item.site), ct_color(CTColor::Reset));
        ct_quarantine_push(
            {item.ptr, item.size, static_cast<unsigned char>(CtReleaseApi::Free), item.kind});
        break;
    }
}

// Lists the live blocks into ct_scan_ranges, sorted, and clears their marks. False when
// they outnumber the buffers reserved before the threads were suspended. ct_alloc_lock held.
CT_NODISCARD CT_NOINSTR static bool ct_scan_list_blocks(void)
{
    size_t count = 0;
    for (size_t i = 0; i < ct_alloc_table_size; ++i)
    {
        struct ct_alloc_entry* entry = &ct_alloc_table[i];
        if (entry->state != CT_ENTRY_USED)
        {
            continue;
        }
        if (count == ct_scan_ranges_capacity || count == ct_scan_pending_capacity)
        {
            return false;
        }
        entry->mark = 0;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(entry->ptr);
        ct_scan_ranges[count++] = {begin, begin + entry->size, entry};
    }
    std::sort(ct_scan_ranges, ct_scan_ranges + count,
              [](const struct ct_block_range& left, const struct ct_block_range& right)
              { return left.begin < right.begin; });
    ct_scan_range_count = count;
    ct_scan_pending_count = 0;
    return true;
}

// A mapping may be unreadable (PROT_NONE): its words are copied through the kernel, which
// fails on such a page instead of faulting. That page may hold the only reference to a
// block, so the pass is then marked incomplete.
CT_NOINSTR static void ct_scan_mapping_for_marks(const struct ct_alloc_entry& entry,
                                                 uint64_t start_ns, int* incomplete)
{
    uintptr_t words[512];
    const uintptr_t begin = reinterpret_cast<uintptr_t>(entry.ptr);
    const uintptr_t end = begin + entry.size;
    for (uintptr_t at = begin; at < end && !*incomplete; at += sizeof(words))
    {
        const mach_vm_size_t chunk = std::min<uintptr_t>(sizeof(words), end - at);
        mach_vm_size_t copied = 0;
        if (mach_vm_read_overwrite(mach_task_self(), at, chunk,
                                   reinterpret_cast<mach_vm_address_t>(words),
                                   &copied) != KERN_SUCCESS)
        {
            *incomplete = 1;
            return;
        }
        ct_scan_range_for_marks(words, reinterpret_cast<const char*>(words) + copied, start_ns,
                                incomplete);
    }
}

// Scans the contents of every marked block, marking the blocks they point to in turn, so
// that a block reachable only through other blocks is kept.
CT_NOINSTR static void ct_scan_marked_blocks(uint64_t start_ns, int* incomplete)
{
    while (ct_scan_pending_count > 0 && !*incomplete)
    {
        const struct ct_alloc_entry* entry = ct_scan_pending[--ct_scan_pending_count];
        if (entry->kind == CT_ALLOC_KIND_MMAP)
        {
            ct_scan_mapping_for_marks(*entry, start_ns, incomplete);
            continue;
        }
        const char* begin = static_cast<const char*>(entry->ptr);
        ct_scan_range_for_marks(begin, begin + entry->size, start_ns, incomplete);
    }
}

// The blocks a pass releases, kept across passes and grown before the threads are
// suspended. Only the scan that owns ct_autofree_scan_guard uses it.
static struct ct_autofree_free_item* ct_scan_items = nullptr;
static size_t ct_scan_items_capacity = 0;

// An allocation the scan found no reference to, which it may release. It never releases
// an Objective-C object: the Objective-C runtime owns its memory and reference count.
CT_NODISCARD CT_NOINSTR static bool ct_autofree_scan_may_release(const struct ct_alloc_entry& entry)
{
    return entry.state == CT_ENTRY_USED && entry.mark == 0 && entry.kind != CT_ALLOC_KIND_OBJC;
}

CT_NOINSTR static void ct_autofree_gc_scan(int force, const char* reason)
{
    ct_init_env_once();
    ct_autofree_scan_init_once();
    if (!ct_autofree_scan_enabled.load(std::memory_order_acquire) ||
        !ct_is_enabled(CT_FEATURE_AUTOFREE) || !ct_is_enabled(CT_FEATURE_ALLOC))
    {
        return;
    }
    ct_autofree_scan_guard scan_guard;
    if (!scan_guard.owned)
    {
        return;
    }
    if (!force && !ct_autofree_gc_should_run())
    {
        return;
    }

    // Everything that allocates, releases, logs or calls into dyld happens before the
    // threads are suspended or after they resume: a suspended thread may hold the
    // allocator's lock, the logger's or dyld's, and the scan would wait for it forever
    // (#118). The work buffers are sized to the table beforehand, which holds every live
    // block unless it grows in between: that pass then releases nothing.
    ct_lock_acquire();
    const size_t live = ct_alloc_count;
    const size_t table_size = ct_alloc_table_size;
    ct_lock_release();
    if (live == 0)
    {
        return;
    }
    void* items_buffer = ct_scan_items;
    void* ranges_buffer = ct_scan_ranges;
    void* pending_buffer = ct_scan_pending;
    const bool reserved = ct_reserve(&items_buffer, &ct_scan_items_capacity, table_size,
                                     sizeof(struct ct_autofree_free_item)) &&
                          ct_reserve(&ranges_buffer, &ct_scan_ranges_capacity, table_size,
                                     sizeof(struct ct_block_range)) &&
                          ct_reserve(&pending_buffer, &ct_scan_pending_capacity, table_size,
                                     sizeof(struct ct_alloc_entry*));
    ct_scan_items = static_cast<struct ct_autofree_free_item*>(items_buffer);
    ct_scan_ranges = static_cast<struct ct_block_range*>(ranges_buffer);
    ct_scan_pending = static_cast<struct ct_alloc_entry**>(pending_buffer);
    if (!reserved)
    {
        return;
    }
    const bool scan_globals = ct_autofree_scan_globals.load(std::memory_order_relaxed);
    uint64_t generation = 0;
    if (scan_globals && !ct_list_data_segments(&generation))
    {
        return;
    }

    uint64_t start_ns = ct_time_ns();
    struct ct_scan_threads threads = {};
    if (!ct_list_threads(&threads))
    {
        return;
    }

    ct_lock_acquire();
    ct_suspend_threads(&threads);

    // A pass that cannot see every block and root, or runs out of time, releases nothing.
    int timed_out = !ct_scan_list_blocks();
    const thread_t scan_thread = ct_autofree_scan_thread_port.load(std::memory_order_acquire);
    for (mach_msg_type_number_t i = 0; i < threads.count && !timed_out; ++i)
    {
        const struct ct_scan_thread& thread = threads.items[i];
        if (!thread.readable || thread.port == scan_thread)
        {
            continue;
        }
        if (ct_autofree_scan_regs.load(std::memory_order_relaxed) ||
            ct_autofree_scan_stack.load(std::memory_order_relaxed))
        {
            ct_scan_thread_regs_and_stack_marks(thread, start_ns, &timed_out);
        }
    }
    // Images changed since the segments were listed: some roots may be missing, so this
    // pass releases nothing, as when it runs out of time.
    if (!timed_out && scan_globals)
    {
        if (ct_data_segments_current(generation))
        {
            ct_scan_globals_for_marks(start_ns, &timed_out);
        }
        else
        {
            timed_out = 1;
        }
    }
    ct_scan_marked_blocks(start_ns, &timed_out);

    size_t collected = 0;
    if (!timed_out)
    {
        for (size_t i = 0; i < ct_alloc_table_size && collected < ct_scan_items_capacity; ++i)
        {
            struct ct_alloc_entry* entry = &ct_alloc_table[i];
            if (ct_autofree_scan_may_release(*entry))
            {
                ct_scan_items[collected++] = {entry->ptr, entry->size, entry->site, entry->kind};
                entry->state = CT_ENTRY_AUTOFREED;
                if (ct_alloc_count > 0)
                {
                    --ct_alloc_count;
                }
            }
        }
    }
    ct_lock_release();
    ct_resume_threads(&threads);

    const int debug_level = ct_autofree_scan_debug.load(std::memory_order_relaxed);
    if (debug_level > 1 || (debug_level == 1 && (timed_out || collected > 0)))
    {
        ct_log(CTLevel::Warn, "{}ct: scan({}) done timed_out={} free_count={}{}\n",
               ct_color(CTColor::BgBrightYellow), reason ? reason : "periodic", timed_out,
               collected, ct_color(CTColor::Reset));
    }
    for (size_t i = 0; i < collected; ++i)
    {
        ct_autofree_do_free(ct_scan_items[i]);
    }
}

CT_NOINSTR static void ct_autofree_scan_sleep(uint64_t ns)
{
    if (ns == 0)
    {
        return;
    }
    struct timespec req;
    req.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
    req.tv_nsec = static_cast<long>(ns % 1000000000ULL);
    while (nanosleep(&req, &req) == -1 && errno == EINTR)
    {
    }
}

CT_NOINSTR static void* ct_autofree_scan_thread_main(void*)
{
    ct_init_env_once();
    ct_autofree_scan_init_once();
    if (!ct_autofree_scan_enabled.load(std::memory_order_acquire) ||
        !ct_autofree_scan_start.load(std::memory_order_acquire))
    {
        return nullptr;
    }
    // The send right is kept for the life of the thread, which is the process's.
    ct_autofree_scan_thread_port.store(mach_thread_self(), std::memory_order_release);

    for (;;)
    {
        ct_autofree_gc_scan(0, "periodic");
        uint64_t interval = ct_autofree_scan_period_ns.load(std::memory_order_relaxed);
        if (interval == 0)
        {
            interval = 1000000000ULL;
        }
        ct_autofree_scan_sleep(interval);
    }
    return nullptr;
}

CT_NOINSTR static void ct_autofree_scan_start_thread(void)
{
    int expected = 0;
    if (!ct_autofree_scan_thread_started.compare_exchange_strong(expected, 1,
                                                                 std::memory_order_acq_rel))
    {
        return;
    }
    if (pthread_create(&ct_autofree_scan_thread, nullptr, ct_autofree_scan_thread_main, nullptr) ==
        0)
    {
        pthread_detach(ct_autofree_scan_thread);
    }
}
#else
CT_NODISCARD CT_NOINSTR int ct_autofree_scan_for_ptr(void*, size_t)
{
    return 0;
}

CT_NOINSTR static void ct_autofree_gc_scan(int, const char*) {}

CT_NOINSTR static void ct_autofree_scan_start_thread(void) {}
#endif

CT_NOINSTR __attribute__((constructor)) static void ct_autofree_scan_ctor(void)
{
    ct_init_env_once();
    ct_autofree_scan_init_once();
    if (ct_autofree_scan_start.load(std::memory_order_acquire))
    {
        ct_autofree_scan_start_thread();
        ct_autofree_gc_scan(1, "startup");
    }
}

CT_NODISCARD CT_NOINSTR int ct_autofree_scan_checks_pointers(void)
{
    return ct_autofree_scan_enabled.load(std::memory_order_acquire) &&
           ct_autofree_scan_ptr.load(std::memory_order_acquire);
}
