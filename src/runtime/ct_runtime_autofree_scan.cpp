// SPDX-License-Identifier: Apache-2.0
//
// Conservative auto-free scan: decides whether an allocation the compiler proved
// unused is still reachable from the mutator's roots (thread registers, thread
// stacks and the images' __DATA segments) before the runtime releases it.
//
// The scan suspends every other thread while it reads their registers and stacks,
// so it only exists where that is possible; elsewhere the entry points below are
// no-ops and every auto-free decision rests on the compile-time analysis alone.
#include "ct_runtime_alloc_internal.h"

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

CT_NODISCARD CT_NOINSTR static struct ct_alloc_entry*
ct_table_find_entry_containing(const void* ptr)
{
    if (!ptr)
    {
        return nullptr;
    }
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    for (size_t i = 0; i < ct_alloc_table_size; ++i)
    {
        struct ct_alloc_entry* entry = &ct_alloc_table[i];
        if (entry->state != CT_ENTRY_USED)
        {
            continue;
        }
        if (!entry->ptr || entry->size == 0)
        {
            continue;
        }
        uintptr_t base = reinterpret_cast<uintptr_t>(entry->ptr);
        if (addr >= base && (addr - base) < entry->size)
        {
            return entry;
        }
    }
    return nullptr;
}

CT_NOINSTR static void ct_autofree_mark_value(uintptr_t value)
{
    if (!value)
    {
        return;
    }
    struct ct_alloc_entry* entry = ct_table_find_entry(reinterpret_cast<const void*>(value));
    if (entry && entry->state == CT_ENTRY_USED)
    {
        entry->mark = 1;
        return;
    }
    if (!ct_autofree_scan_interior.load(std::memory_order_relaxed))
    {
        return;
    }
    entry = ct_table_find_entry_containing(reinterpret_cast<const void*>(value));
    if (entry && entry->state == CT_ENTRY_USED)
    {
        entry->mark = 1;
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

CT_NODISCARD CT_NOINSTR static int ct_scan_thread_stack_for_ptr(thread_t thread, uintptr_t base,
                                                                size_t size, uint64_t start_ns)
{
    pthread_t pthread = pthread_from_mach_thread_np(thread);
    if (!pthread)
    {
        return 0;
    }
    void* stack_addr = pthread_get_stackaddr_np(pthread);
    size_t stack_size = pthread_get_stacksize_np(pthread);
    if (!stack_addr || !stack_size)
    {
        return 0;
    }

    uintptr_t top = reinterpret_cast<uintptr_t>(stack_addr);
    uintptr_t bottom = top - stack_size;
    long page_size = sysconf(_SC_PAGESIZE);
    uintptr_t guard = bottom;
    if (page_size > 0 && guard + static_cast<uintptr_t>(page_size) < top)
    {
        guard += static_cast<uintptr_t>(page_size);
    }
    uintptr_t sp = 0;
    if (ct_thread_get_sp(thread, &sp) && sp >= guard && sp < top)
    {
        bottom = sp;
    }
    else
    {
        bottom = guard;
    }
    return ct_scan_range_for_ptr(base, size, reinterpret_cast<void*>(bottom),
                                 reinterpret_cast<void*>(top), start_ns);
}

CT_NOINSTR static void ct_scan_thread_stack_for_marks(thread_t thread, uint64_t start_ns,
                                                      int* timed_out)
{
    pthread_t pthread = pthread_from_mach_thread_np(thread);
    if (!pthread)
    {
        return;
    }
    void* stack_addr = pthread_get_stackaddr_np(pthread);
    size_t stack_size = pthread_get_stacksize_np(pthread);
    if (!stack_addr || !stack_size)
    {
        return;
    }

    uintptr_t top = reinterpret_cast<uintptr_t>(stack_addr);
    uintptr_t bottom = top - stack_size;
    long page_size = sysconf(_SC_PAGESIZE);
    uintptr_t guard = bottom;
    if (page_size > 0 && guard + static_cast<uintptr_t>(page_size) < top)
    {
        guard += static_cast<uintptr_t>(page_size);
    }
    uintptr_t sp = 0;
    if (ct_thread_get_sp(thread, &sp) && sp >= guard && sp < top)
    {
        bottom = sp;
    }
    else
    {
        bottom = guard;
    }
    ct_scan_range_for_marks(reinterpret_cast<void*>(bottom), reinterpret_cast<void*>(top), start_ns,
                            timed_out);
}

// Optimized function: scan regs and stack together in one pass
CT_NOINSTR static void ct_scan_thread_regs_and_stack_marks(thread_t thread, uint64_t start_ns,
                                                           int* timed_out)
{
    if (timed_out && *timed_out)
    {
        return;
    }

    // Scan regs if enabled
    if (ct_autofree_scan_regs.load(std::memory_order_relaxed))
    {
        ct_scan_regs_for_marks(thread, start_ns, timed_out);
    }

    // Then scan stack if enabled and not timed out
    if (ct_autofree_scan_stack.load(std::memory_order_relaxed) && (!timed_out || !*timed_out))
    {
        ct_scan_thread_stack_for_marks(thread, start_ns, timed_out);
    }
}

CT_NODISCARD CT_NOINSTR static int ct_scan_globals_for_ptr(uintptr_t base, size_t size,
                                                           uint64_t start_ns)
{
    uint32_t image_count = _dyld_image_count();
    for (uint32_t i = 0; i < image_count; ++i)
    {
        const mach_header_64* header =
            reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
        if (!header || header->magic != MH_MAGIC_64)
        {
            continue;
        }
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        const load_command* cmd = reinterpret_cast<const load_command*>(
            reinterpret_cast<const char*>(header) + sizeof(mach_header_64));
        for (uint32_t c = 0; c < header->ncmds; ++c)
        {
            if (cmd->cmd == LC_SEGMENT_64)
            {
                const segment_command_64* seg = reinterpret_cast<const segment_command_64*>(cmd);
                if (std::strncmp(seg->segname, "__DATA", 6) == 0)
                {
                    uintptr_t seg_start = static_cast<uintptr_t>(seg->vmaddr + slide);
                    uintptr_t seg_end = seg_start + static_cast<uintptr_t>(seg->vmsize);
                    if (ct_scan_range_for_ptr(base, size, reinterpret_cast<void*>(seg_start),
                                              reinterpret_cast<void*>(seg_end), start_ns))
                    {
                        return 1;
                    }
                }
            }
            cmd = reinterpret_cast<const load_command*>(reinterpret_cast<const char*>(cmd) +
                                                        cmd->cmdsize);
            if (ct_scan_time_exceeded(start_ns))
            {
                return 1;
            }
        }
    }
    return 0;
}

CT_NOINSTR static void ct_scan_globals_for_marks(uint64_t start_ns, int* timed_out)
{
    uint32_t image_count = _dyld_image_count();
    for (uint32_t i = 0; i < image_count; ++i)
    {
        const mach_header_64* header =
            reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
        if (!header || header->magic != MH_MAGIC_64)
        {
            continue;
        }
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        const load_command* cmd = reinterpret_cast<const load_command*>(
            reinterpret_cast<const char*>(header) + sizeof(mach_header_64));
        for (uint32_t c = 0; c < header->ncmds; ++c)
        {
            if (cmd->cmd == LC_SEGMENT_64)
            {
                const segment_command_64* seg = reinterpret_cast<const segment_command_64*>(cmd);
                if (std::strncmp(seg->segname, "__DATA", 6) == 0)
                {
                    uintptr_t seg_start = static_cast<uintptr_t>(seg->vmaddr + slide);
                    uintptr_t seg_end = seg_start + static_cast<uintptr_t>(seg->vmsize);
                    ct_scan_range_for_marks(reinterpret_cast<void*>(seg_start),
                                            reinterpret_cast<void*>(seg_end), start_ns, timed_out);
                    if (timed_out && *timed_out)
                    {
                        return;
                    }
                }
            }
            cmd = reinterpret_cast<const load_command*>(reinterpret_cast<const char*>(cmd) +
                                                        cmd->cmdsize);
            if (ct_scan_time_exceeded(start_ns))
            {
                if (timed_out)
                {
                    *timed_out = 1;
                }
                return;
            }
        }
    }
}

CT_NODISCARD CT_NOINSTR int ct_autofree_scan_for_ptr(void* ptr, size_t size)
{
    if (!ct_autofree_scan_should_run())
    {
        return 0;
    }

    uint64_t start_ns = ct_time_ns();
    uintptr_t base = reinterpret_cast<uintptr_t>(ptr);

    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t thread_count = 0;
    if (task_threads(mach_task_self(), &threads, &thread_count) != KERN_SUCCESS)
    {
        return 0;
    }

    thread_t self_thread = mach_thread_self();
    for (mach_msg_type_number_t i = 0; i < thread_count; ++i)
    {
        if (threads[i] == self_thread)
        {
            continue;
        }
        thread_suspend(threads[i]);
    }

    int found = 0;
    for (mach_msg_type_number_t i = 0; i < thread_count && !found; ++i)
    {
        thread_t thread = threads[i];
        if (ct_autofree_scan_regs.load(std::memory_order_relaxed))
        {
            if (ct_scan_regs_for_ptr(thread, base, size, start_ns))
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

    if (!found && ct_autofree_scan_globals.load(std::memory_order_relaxed))
    {
        if (ct_scan_globals_for_ptr(base, size, start_ns))
        {
            found = 1;
        }
    }

    for (mach_msg_type_number_t i = 0; i < thread_count; ++i)
    {
        if (threads[i] != self_thread)
        {
            thread_resume(threads[i]);
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                  thread_count * sizeof(thread_t));
    mach_port_deallocate(mach_task_self(), self_thread);

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
        ::operator delete(item.ptr);
        break;
    case CT_ALLOC_KIND_NEW_ARRAY:
        if (ct_is_enabled(CT_FEATURE_SHADOW))
        {
            ct_shadow_poison_range(item.ptr, item.size);
        }
        ct_log(CTLevel::Warn, "{}auto-free(scan) kind={} ptr={:p} size={} site={}{}\n",
               ct_color(CTColor::BgBrightYellow), ct_alloc_kind_label(item.kind), item.ptr,
               item.size, ct_site_name(item.site), ct_color(CTColor::Reset));
        ::operator delete[](item.ptr);
        break;
    case CT_ALLOC_KIND_MMAP:
        if (ct_is_enabled(CT_FEATURE_SHADOW))
        {
            ct_shadow_poison_range(item.ptr, item.size);
        }
        ct_log(CTLevel::Warn, "{}auto-free(scan) kind={} ptr={:p} size={} site={}{}\n",
               ct_color(CTColor::BgBrightYellow), ct_alloc_kind_label(item.kind), item.ptr,
               item.size, ct_site_name(item.site), ct_color(CTColor::Reset));
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
            if (ct_is_enabled(CT_FEATURE_SHADOW))
            {
                ct_shadow_poison_range(item.ptr, item.size);
            }
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
        free(item.ptr);
        break;
    }
}

// Claims ct_autofree_scan_in_progress for the lifetime of a GC scan and releases it on
// every exit path, so an early return can never leave the flag set and starve later scans.
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

    uint64_t start_ns = ct_time_ns();
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t thread_count = 0;
    if (task_threads(mach_task_self(), &threads, &thread_count) != KERN_SUCCESS)
    {
        return;
    }

    ct_lock_acquire();
    thread_t self_thread = mach_thread_self();
    for (mach_msg_type_number_t i = 0; i < thread_count; ++i)
    {
        if (threads[i] == self_thread)
        {
            continue;
        }
        thread_suspend(threads[i]);
    }

    // Single pass: reset marks for used entries
    for (size_t i = 0; i < ct_alloc_table_size; ++i)
    {
        if (ct_alloc_table[i].state == CT_ENTRY_USED)
        {
            ct_alloc_table[i].mark = 0;
        }
    }

    int timed_out = 0;
    for (mach_msg_type_number_t i = 0; i < thread_count && !timed_out; ++i)
    {
        if (ct_autofree_scan_regs.load(std::memory_order_relaxed) ||
            ct_autofree_scan_stack.load(std::memory_order_relaxed))
        {
            // Optimized: combine regs and stack scan
            ct_scan_thread_regs_and_stack_marks(threads[i], start_ns, &timed_out);
        }
    }
    if (!timed_out && ct_autofree_scan_globals.load(std::memory_order_relaxed))
    {
        ct_scan_globals_for_marks(start_ns, &timed_out);
    }

    // Single pass: count and collect unmarked entries
    size_t to_free_count = 0;
    size_t idx = 0;
    struct ct_autofree_free_item* items = nullptr;

    if (!timed_out)
    {
        for (size_t i = 0; i < ct_alloc_table_size; ++i)
        {
            if (ct_alloc_table[i].state == CT_ENTRY_USED && ct_alloc_table[i].mark == 0)
            {
                ++to_free_count;
            }
        }
    }

    ct_lock_release();

    if (!timed_out && to_free_count > 0)
    {
        items = static_cast<struct ct_autofree_free_item*>(
            std::malloc(sizeof(struct ct_autofree_free_item) * to_free_count));
    }

    ct_lock_acquire();
    if (!timed_out && items)
    {
        for (size_t i = 0; i < ct_alloc_table_size && idx < to_free_count; ++i)
        {
            struct ct_alloc_entry* entry = &ct_alloc_table[i];
            if (entry->state == CT_ENTRY_USED && entry->mark == 0)
            {
                items[idx].ptr = entry->ptr;
                items[idx].size = entry->size;
                items[idx].site = entry->site;
                items[idx].kind = entry->kind;
                ++idx;
                entry->state = CT_ENTRY_AUTOFREED;
                if (ct_alloc_count > 0)
                {
                    --ct_alloc_count;
                }
            }
        }
    }
    ct_lock_release();

    const int debug_level = ct_autofree_scan_debug.load(std::memory_order_relaxed);
    if (debug_level > 1 || (debug_level == 1 && (timed_out || to_free_count > 0)))
    {
        ct_log(CTLevel::Warn, "{}ct: scan({}) done timed_out={} free_count={}{}\n",
               ct_color(CTColor::BgBrightYellow), reason ? reason : "periodic", timed_out,
               to_free_count, ct_color(CTColor::Reset));
    }

    if (!timed_out && items)
    {
        for (size_t i = 0; i < idx; ++i)
        {
            ct_autofree_do_free(items[i]);
        }
    }
    if (items)
    {
        std::free(items);
    }

    for (mach_msg_type_number_t i = 0; i < thread_count; ++i)
    {
        if (threads[i] != self_thread)
        {
            thread_resume(threads[i]);
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                  thread_count * sizeof(thread_t));
    mach_port_deallocate(mach_task_self(), self_thread);
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
