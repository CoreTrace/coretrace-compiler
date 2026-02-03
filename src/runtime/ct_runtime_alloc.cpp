#include "ct_runtime_internal.h"

#include <cstdlib>
#include <new>
#include <cstring>
#include <format>
#include <new>
#include <chrono>
#include <time.h>
#include <sys/mman.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

struct ct_alloc_entry
{
    void* ptr;
    size_t size;
    size_t req_size;
    const char* site;
    unsigned char state;
    unsigned char kind;
    unsigned char mark;
};

struct ct_autofree_free_item
{
    void* ptr;
    size_t size;
    const char* site;
    unsigned char kind;
};

enum
{
    CT_ALLOC_KIND_MALLOC = 0,
    CT_ALLOC_KIND_NEW = 1,
    CT_ALLOC_KIND_NEW_ARRAY = 2,
    CT_ALLOC_KIND_MMAP = 3,
    CT_ALLOC_KIND_SBRK = 4
};

#define CT_ALLOC_TABLE_BITS 16u
#define CT_ALLOC_TABLE_MAX_BITS 20u
#define CT_ALLOC_TABLE_SIZE (1u << CT_ALLOC_TABLE_BITS)

static struct ct_alloc_entry ct_alloc_table_storage[CT_ALLOC_TABLE_SIZE];
static struct ct_alloc_entry* ct_alloc_table = ct_alloc_table_storage;
static size_t ct_alloc_table_bits = CT_ALLOC_TABLE_BITS;
static size_t ct_alloc_table_size = CT_ALLOC_TABLE_SIZE;
static size_t ct_alloc_table_mask = CT_ALLOC_TABLE_SIZE - 1u;
static size_t ct_alloc_count = 0;
static int ct_alloc_lock = 0;
static int ct_alloc_table_full_logged = 0;
static int ct_autofree_scan_initialized = 0;
static int ct_autofree_scan_enabled = 0;
static int ct_autofree_scan_start = 0;
static int ct_autofree_scan_in_progress = 0;
static int ct_autofree_scan_stack = 1;
static int ct_autofree_scan_regs = 1;
static int ct_autofree_scan_globals = 1;
static int ct_autofree_scan_interior = 1;
static int ct_autofree_scan_debug = 0;
static int ct_autofree_scan_ptr = 1;
static uint64_t ct_autofree_scan_interval_ns = 0;
static uint64_t ct_autofree_scan_period_ns = 0;
static uint64_t ct_autofree_scan_budget_ns = 0;
static uint64_t ct_autofree_scan_last_ns = 0;
static uint64_t ct_autofree_scan_last_gc_ns = 0;
static pthread_t ct_autofree_scan_thread;
static int ct_autofree_scan_thread_started = 0;

extern "C"
{
    CT_NOINSTR void __ct_autofree(void* ptr);
    CT_NOINSTR void __ct_autofree_delete(void* ptr);
    CT_NOINSTR void __ct_autofree_delete_array(void* ptr);
    CT_NOINSTR void __ct_autofree_munmap(void* ptr);
    CT_NOINSTR void __ct_autofree_sbrk(void* ptr);
}

CT_NOINSTR void ct_lock_acquire(void)
{
    while (__atomic_exchange_n(&ct_alloc_lock, 1, __ATOMIC_ACQUIRE) != 0)
    {
    }
}

CT_NOINSTR void ct_lock_release(void)
{
    __atomic_store_n(&ct_alloc_lock, 0, __ATOMIC_RELEASE);
}

CT_NODISCARD CT_NOINSTR static size_t ct_hash_ptr(const void* ptr, size_t mask)
{
    uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
    value ^= value >> 4;
    value ^= value >> 9;
    return static_cast<size_t>(value) & mask;
}

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

CT_NOINSTR static void ct_autofree_scan_init_once(void)
{
    if (ct_autofree_scan_initialized)
    {
        return;
    }
    ct_autofree_scan_initialized = 1;
    ct_autofree_scan_enabled = ct_env_flag("CT_AUTOFREE_SCAN", 0);
    ct_autofree_scan_start = ct_env_flag("CT_AUTOFREE_SCAN_START", 0);
    ct_autofree_scan_stack = ct_env_flag("CT_AUTOFREE_SCAN_STACK", 1);
    ct_autofree_scan_regs = ct_env_flag("CT_AUTOFREE_SCAN_REGS", 1);
    ct_autofree_scan_globals = ct_env_flag("CT_AUTOFREE_SCAN_GLOBALS", 1);
    ct_autofree_scan_interior = ct_env_flag("CT_AUTOFREE_SCAN_INTERIOR", 1);
    ct_autofree_scan_debug = ct_env_flag("CT_DEBUG_AUTOFREE_SCAN", 0);
    ct_autofree_scan_ptr = ct_env_flag("CT_AUTOFREE_SCAN_PTR", 1);
    ct_autofree_scan_interval_ns =
        ct_env_u64("CT_AUTOFREE_SCAN_INTERVAL_MS", 0) * 1000000ULL;

    const uint64_t period_ns = ct_env_u64("CT_AUTOFREE_SCAN_PERIOD_NS", 0);
    const uint64_t period_us = ct_env_u64("CT_AUTOFREE_SCAN_PERIOD_US", 0);
    const double period_ms = ct_env_f64("CT_AUTOFREE_SCAN_PERIOD_MS", 0.0);

    if (period_ns)
    {
        ct_autofree_scan_period_ns = period_ns;
    }
    else if (period_us)
    {
        ct_autofree_scan_period_ns = period_us * 1000ULL;
    }
    else if (period_ms > 0.0)
    {
        ct_autofree_scan_period_ns = static_cast<uint64_t>(period_ms * 1000000.0);
    }
    else
    {
        ct_autofree_scan_period_ns = 0;
    }

    const uint64_t budget_ns = ct_env_u64("CT_AUTOFREE_SCAN_BUDGET_NS", 0);
    const uint64_t budget_us = ct_env_u64("CT_AUTOFREE_SCAN_BUDGET_US", 0);
    const double budget_ms = ct_env_f64("CT_AUTOFREE_SCAN_BUDGET_MS", 5.0);

    if (budget_ns)
    {
        ct_autofree_scan_budget_ns = budget_ns;
    }
    else if (budget_us)
    {
        ct_autofree_scan_budget_ns = budget_us * 1000ULL;
    }
    else if (budget_ms > 0.0)
    {
        ct_autofree_scan_budget_ns = static_cast<uint64_t>(budget_ms * 1000000.0);
    }
    else
    {
        ct_autofree_scan_budget_ns = 0;
    }
    if (ct_autofree_scan_start)
    {
        ct_autofree_scan_enabled = 1;
        if (ct_autofree_scan_period_ns == 0)
        {
            ct_autofree_scan_period_ns = 1000000000ULL;
        }
    }
}

CT_NODISCARD CT_NOINSTR static int ct_autofree_scan_should_run(void)
{
    if (!ct_autofree_scan_enabled)
    {
        return 0;
    }
    if (ct_autofree_scan_in_progress)
    {
        return 0;
    }
    uint64_t now = ct_time_ns();
    if (ct_autofree_scan_interval_ns != 0 &&
        now - ct_autofree_scan_last_ns < ct_autofree_scan_interval_ns)
    {
        return 0;
    }
    ct_autofree_scan_last_ns = now;
    return 1;
}

CT_NODISCARD CT_NOINSTR static int ct_scan_time_exceeded(uint64_t start_ns)
{
    if (ct_autofree_scan_budget_ns == 0)
    {
        return 0;
    }
    return (ct_time_ns() - start_ns) >= ct_autofree_scan_budget_ns;
}

CT_NODISCARD CT_NOINSTR static struct ct_alloc_entry* ct_table_find_entry(const void* ptr)
{
    size_t idx = ct_hash_ptr(ptr, ct_alloc_table_mask);
    for (size_t i = 0; i < ct_alloc_table_size; ++i)
    {
        size_t pos = (idx + i) & ct_alloc_table_mask;
        struct ct_alloc_entry* entry = &ct_alloc_table[pos];
        if (entry->state == CT_ENTRY_EMPTY)
        {
            return nullptr;
        }
        if (entry->ptr == ptr &&
            (entry->state == CT_ENTRY_USED || entry->state == CT_ENTRY_FREED ||
             entry->state == CT_ENTRY_AUTOFREED))
        {
            return entry;
        }
    }
    return nullptr;
}

CT_NODISCARD CT_NOINSTR static struct ct_alloc_entry* ct_table_find_entry_containing(
    const void* ptr)
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
    struct ct_alloc_entry* entry =
        ct_table_find_entry(reinterpret_cast<const void*>(value));
    if (entry && entry->state == CT_ENTRY_USED)
    {
        entry->mark = 1;
        return;
    }
    if (!ct_autofree_scan_interior)
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
    if (!ct_autofree_scan_enabled || !ct_autofree_scan_start)
    {
        return 0;
    }
    if (ct_autofree_scan_period_ns == 0)
    {
        return 1;
    }
    uint64_t now = ct_time_ns();
    if (now - ct_autofree_scan_last_gc_ns < ct_autofree_scan_period_ns)
    {
        return 0;
    }
    ct_autofree_scan_last_gc_ns = now;
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
        if ((++counter & 0xFF) == 0)
        {
            if (ct_scan_time_exceeded(start_ns))
            {
                return 1;
            }
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
        if ((++counter & 0xFF) == 0)
        {
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

CT_NOINSTR static void ct_scan_regs_for_marks(thread_t thread, uint64_t start_ns,
                                              int* timed_out)
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

CT_NODISCARD CT_NOINSTR static int ct_scan_thread_stack_for_ptr(thread_t thread,
                                                                uintptr_t base, size_t size,
                                                                uint64_t start_ns)
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
    ct_scan_range_for_marks(reinterpret_cast<void*>(bottom), reinterpret_cast<void*>(top),
                            start_ns, timed_out);
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
        const load_command* cmd =
            reinterpret_cast<const load_command*>(reinterpret_cast<const char*>(header) +
                                                  sizeof(mach_header_64));
        for (uint32_t c = 0; c < header->ncmds; ++c)
        {
            if (cmd->cmd == LC_SEGMENT_64)
            {
                const segment_command_64* seg =
                    reinterpret_cast<const segment_command_64*>(cmd);
                if (std::strncmp(seg->segname, "__DATA", 6) == 0)
                {
                    uintptr_t seg_start = static_cast<uintptr_t>(seg->vmaddr + slide);
                    uintptr_t seg_end = seg_start + static_cast<uintptr_t>(seg->vmsize);
                    if (ct_scan_range_for_ptr(base, size,
                                              reinterpret_cast<void*>(seg_start),
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
        const load_command* cmd =
            reinterpret_cast<const load_command*>(reinterpret_cast<const char*>(header) +
                                                  sizeof(mach_header_64));
        for (uint32_t c = 0; c < header->ncmds; ++c)
        {
            if (cmd->cmd == LC_SEGMENT_64)
            {
                const segment_command_64* seg =
                    reinterpret_cast<const segment_command_64*>(cmd);
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

CT_NODISCARD CT_NOINSTR static int ct_autofree_scan_for_ptr(void* ptr, size_t size)
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
        if (ct_autofree_scan_regs)
        {
            if (ct_scan_regs_for_ptr(thread, base, size, start_ns))
            {
                found = 1;
                break;
            }
        }
        if (ct_autofree_scan_stack)
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

    if (!found && ct_autofree_scan_globals)
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

    if (ct_autofree_scan_debug)
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
        if (ct_shadow_enabled)
        {
            ct_shadow_poison_range(item.ptr, item.size);
        }
        ct_log(CTLevel::Warn, "{}auto-free(scan) kind={} ptr={:p} size={} site={}{}\n",
               ct_color(CTColor::BgBrightYellow), ct_alloc_kind_label(item.kind), item.ptr,
               item.size, ct_site_name(item.site), ct_color(CTColor::Reset));
        ::operator delete(item.ptr);
        break;
    case CT_ALLOC_KIND_NEW_ARRAY:
        if (ct_shadow_enabled)
        {
            ct_shadow_poison_range(item.ptr, item.size);
        }
        ct_log(CTLevel::Warn, "{}auto-free(scan) kind={} ptr={:p} size={} site={}{}\n",
               ct_color(CTColor::BgBrightYellow), ct_alloc_kind_label(item.kind), item.ptr,
               item.size, ct_site_name(item.site), ct_color(CTColor::Reset));
        ::operator delete[](item.ptr);
        break;
    case CT_ALLOC_KIND_MMAP:
        if (ct_shadow_enabled)
        {
            ct_shadow_poison_range(item.ptr, item.size);
        }
        ct_log(CTLevel::Warn, "{}auto-free(scan) kind={} ptr={:p} size={} site={}{}\n",
               ct_color(CTColor::BgBrightYellow), ct_alloc_kind_label(item.kind), item.ptr,
               item.size, ct_site_name(item.site), ct_color(CTColor::Reset));
        (void)munmap(item.ptr, item.size);
        break;
    case CT_ALLOC_KIND_SBRK:
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    {
        void* current = sbrk(0);
        if (current != (void*)-1 &&
            static_cast<char*>(item.ptr) + static_cast<ptrdiff_t>(item.size) == current)
        {
            (void)sbrk(-static_cast<intptr_t>(item.size));
            if (ct_shadow_enabled)
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
    case CT_ALLOC_KIND_MALLOC:
    default:
        if (ct_shadow_enabled)
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

CT_NOINSTR static void ct_autofree_gc_scan(int force, const char* reason)
{
    ct_init_env_once();
    ct_autofree_scan_init_once();
    if (!ct_autofree_scan_enabled || !ct_autofree_enabled || ct_disable_alloc)
    {
        return;
    }
    if (ct_autofree_scan_in_progress)
    {
        return;
    }
    if (!force && !ct_autofree_gc_should_run())
    {
        return;
    }

    ct_autofree_scan_in_progress = 1;

    uint64_t start_ns = ct_time_ns();
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t thread_count = 0;
    if (task_threads(mach_task_self(), &threads, &thread_count) != KERN_SUCCESS)
    {
        ct_autofree_scan_in_progress = 0;
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
        if (ct_autofree_scan_regs)
        {
            ct_scan_regs_for_marks(threads[i], start_ns, &timed_out);
        }
        if (ct_autofree_scan_stack && !timed_out)
        {
            ct_scan_thread_stack_for_marks(threads[i], start_ns, &timed_out);
        }
    }
    if (!timed_out && ct_autofree_scan_globals)
    {
        ct_scan_globals_for_marks(start_ns, &timed_out);
    }

    size_t to_free_count = 0;
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

    struct ct_autofree_free_item* items = nullptr;
    if (!timed_out && to_free_count > 0)
    {
        items = static_cast<struct ct_autofree_free_item*>(
            std::malloc(sizeof(struct ct_autofree_free_item) * to_free_count));
    }

    size_t idx = 0;
    if (!timed_out && to_free_count > 0 && items)
    {
        for (size_t i = 0; i < ct_alloc_table_size; ++i)
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

    if (ct_autofree_scan_debug)
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

    ct_autofree_scan_in_progress = 0;
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
    if (!ct_autofree_scan_enabled || !ct_autofree_scan_start)
    {
        return nullptr;
    }

    for (;;)
    {
        ct_autofree_gc_scan(0, "periodic");
        uint64_t interval = ct_autofree_scan_period_ns;
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
    if (ct_autofree_scan_thread_started)
    {
        return;
    }
    ct_autofree_scan_thread_started = 1;
    if (pthread_create(&ct_autofree_scan_thread, nullptr, ct_autofree_scan_thread_main, nullptr) ==
        0)
    {
        pthread_detach(ct_autofree_scan_thread);
    }
}
#else
CT_NODISCARD CT_NOINSTR static int ct_autofree_scan_for_ptr(void*, size_t)
{
    return 0;
}

CT_NOINSTR static void ct_autofree_gc_scan(int, const char*)
{
}

CT_NOINSTR static void ct_autofree_scan_start_thread(void)
{
}
#endif

CT_NOINSTR __attribute__((constructor)) static void ct_autofree_scan_ctor(void)
{
    ct_init_env_once();
    ct_autofree_scan_init_once();
    if (ct_autofree_scan_start)
    {
        ct_autofree_scan_start_thread();
        ct_autofree_gc_scan(1, "startup");
    }
}

CT_NODISCARD CT_NOINSTR static int ct_alloc_rehash_entry(struct ct_alloc_entry* table, size_t mask,
                                                         size_t size,
                                                         const struct ct_alloc_entry* entry)
{
    size_t idx = ct_hash_ptr(entry->ptr, mask);
    for (size_t i = 0; i < size; ++i)
    {
        size_t pos = (idx + i) & mask;
        struct ct_alloc_entry* slot = &table[pos];

        if (slot->state == CT_ENTRY_EMPTY)
        {
            *slot = *entry;
            return 1;
        }
    }
    return 0;
}

CT_NODISCARD CT_NOINSTR static int ct_alloc_grow_locked(void)
{
    if (ct_alloc_table_bits >= CT_ALLOC_TABLE_MAX_BITS)
        return 0;

    size_t new_bits = ct_alloc_table_bits + 1u;
    size_t new_size = 1u << new_bits;
    auto* new_table =
        static_cast<struct ct_alloc_entry*>(std::malloc(sizeof(struct ct_alloc_entry) * new_size));
    if (!new_table)
        return 0;

    std::memset(new_table, 0, sizeof(struct ct_alloc_entry) * new_size);

    size_t new_mask = new_size - 1u;
    size_t new_count = 0;
    for (size_t i = 0; i < ct_alloc_table_size; ++i)
    {
        struct ct_alloc_entry* entry = &ct_alloc_table[i];

        if (entry->state != CT_ENTRY_USED && entry->state != CT_ENTRY_FREED &&
            entry->state != CT_ENTRY_AUTOFREED)
            continue;

        if (ct_alloc_rehash_entry(new_table, new_mask, new_size, entry))
        {
            if (entry->state == CT_ENTRY_USED)
                ++new_count;
        }
    }

    if (ct_alloc_table != ct_alloc_table_storage)
        std::free(ct_alloc_table);

    ct_alloc_table = new_table;
    ct_alloc_table_bits = new_bits;
    ct_alloc_table_size = new_size;
    ct_alloc_table_mask = new_mask;
    ct_alloc_count = new_count;
    ct_alloc_table_full_logged = 0;

    return 1;
}

CT_NODISCARD CT_NOINSTR int ct_table_insert(void* ptr, size_t req_size, size_t size,
                                            const char* site, unsigned char kind)
{
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        size_t idx = ct_hash_ptr(ptr, ct_alloc_table_mask);
        size_t tombstone = static_cast<size_t>(-1);

        for (size_t i = 0; i < ct_alloc_table_size; ++i)
        {
            size_t pos = (idx + i) & ct_alloc_table_mask;
            struct ct_alloc_entry* entry = &ct_alloc_table[pos];

            if (entry->state == CT_ENTRY_USED)
            {
                if (entry->ptr == ptr)
                {
                    entry->size = size;
                    entry->req_size = req_size;
                    entry->site = site;
                    entry->kind = kind;
                    entry->mark = 0;
                    return 1;
                }
                continue;
            }

            if ((entry->state == CT_ENTRY_TOMB || entry->state == CT_ENTRY_FREED ||
                 entry->state == CT_ENTRY_AUTOFREED) &&
                tombstone == static_cast<size_t>(-1))
            {
                tombstone = pos;
                continue;
            }

            if (entry->state == CT_ENTRY_EMPTY)
            {
                if (tombstone != static_cast<size_t>(-1))
                {
                    entry = &ct_alloc_table[tombstone];
                }
                entry->ptr = ptr;
                entry->size = size;
                entry->req_size = req_size;
                entry->site = site;
                entry->kind = kind;
                entry->mark = 0;
                entry->state = CT_ENTRY_USED;
                ++ct_alloc_count;
                return 1;
            }
        }

        if (tombstone != static_cast<size_t>(-1))
        {
            struct ct_alloc_entry* entry = &ct_alloc_table[tombstone];
            entry->ptr = ptr;
            entry->size = size;
            entry->req_size = req_size;
            entry->site = site;
            entry->kind = kind;
            entry->mark = 0;
            entry->state = CT_ENTRY_USED;
            ++ct_alloc_count;
            return 1;
        }

        if (!ct_alloc_grow_locked())
        {
            return 0;
        }
    }

    return 0;
}

CT_NODISCARD CT_NOINSTR int ct_table_remove(void* ptr, size_t* size_out, size_t* req_size_out,
                                            const char** site_out)
{
    size_t idx = ct_hash_ptr(ptr, ct_alloc_table_mask);

    for (size_t i = 0; i < ct_alloc_table_size; ++i)
    {
        size_t pos = (idx + i) & ct_alloc_table_mask;
        struct ct_alloc_entry* entry = &ct_alloc_table[pos];

        if (entry->state == CT_ENTRY_EMPTY)
        {
            return 0;
        }
        if (entry->state == CT_ENTRY_USED && entry->ptr == ptr)
        {
            if (size_out)
            {
                *size_out = entry->size;
            }
            if (req_size_out)
            {
                *req_size_out = entry->req_size;
            }
            if (site_out)
            {
                *site_out = entry->site;
            }
            if (ct_alloc_count > 0)
            {
                --ct_alloc_count;
            }
            entry->state = CT_ENTRY_FREED;
            return 1;
        }
        if ((entry->state == CT_ENTRY_FREED || entry->state == CT_ENTRY_AUTOFREED) &&
            entry->ptr == ptr)
        {
            if (size_out)
            {
                *size_out = entry->size;
            }
            if (req_size_out)
            {
                *req_size_out = entry->req_size;
            }
            if (site_out)
            {
                *site_out = entry->site;
            }
            return -1;
        }
    }

    return 0;
}

CT_NODISCARD CT_NOINSTR int ct_table_remove_autofree(void* ptr, size_t* size_out,
                                                     size_t* req_size_out,
                                                     const char** site_out)
{
    size_t idx = ct_hash_ptr(ptr, ct_alloc_table_mask);

    for (size_t i = 0; i < ct_alloc_table_size; ++i)
    {
        size_t pos = (idx + i) & ct_alloc_table_mask;
        struct ct_alloc_entry* entry = &ct_alloc_table[pos];

        if (entry->state == CT_ENTRY_EMPTY)
        {
            return 0;
        }
        if (entry->state == CT_ENTRY_USED && entry->ptr == ptr)
        {
            if (size_out)
            {
                *size_out = entry->size;
            }
            if (req_size_out)
            {
                *req_size_out = entry->req_size;
            }
            if (site_out)
            {
                *site_out = entry->site;
            }
            if (ct_alloc_count > 0)
            {
                --ct_alloc_count;
            }
            entry->state = CT_ENTRY_AUTOFREED;
            return 1;
        }
        if (entry->state == CT_ENTRY_AUTOFREED && entry->ptr == ptr)
        {
            if (size_out)
            {
                *size_out = entry->size;
            }
            if (req_size_out)
            {
                *req_size_out = entry->req_size;
            }
            if (site_out)
            {
                *site_out = entry->site;
            }
            return -2;
        }
        if (entry->state == CT_ENTRY_FREED && entry->ptr == ptr)
        {
            if (size_out)
            {
                *size_out = entry->size;
            }
            if (req_size_out)
            {
                *req_size_out = entry->req_size;
            }
            if (site_out)
            {
                *site_out = entry->site;
            }
            return -1;
        }
    }

    return 0;
}

CT_NODISCARD CT_NOINSTR int ct_table_lookup(const void* ptr, size_t* size_out, size_t* req_size_out,
                                            const char** site_out, unsigned char* state_out)
{
    size_t idx = ct_hash_ptr(ptr, ct_alloc_table_mask);

    for (size_t i = 0; i < ct_alloc_table_size; ++i)
    {
        size_t pos = (idx + i) & ct_alloc_table_mask;
        struct ct_alloc_entry* entry = &ct_alloc_table[pos];

        if (entry->state == CT_ENTRY_EMPTY)
        {
            return 0;
        }
        if ((entry->state == CT_ENTRY_USED || entry->state == CT_ENTRY_FREED ||
             entry->state == CT_ENTRY_AUTOFREED) &&
            entry->ptr == ptr)
        {
            if (size_out)
            {
                *size_out = entry->size;
            }
            if (req_size_out)
            {
                *req_size_out = entry->req_size;
            }
            if (site_out)
            {
                *site_out = entry->site;
            }
            if (state_out)
            {
                *state_out = entry->state;
            }
            return 1;
        }
    }

    return 0;
}

CT_NODISCARD CT_NOINSTR int ct_table_lookup_containing(const void* ptr, void** base_out,
                                                       size_t* size_out, size_t* req_size_out,
                                                       const char** site_out,
                                                       unsigned char* state_out)
{
    if (!ptr)
        return 0;

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

    for (size_t i = 0; i < ct_alloc_table_size; ++i)
    {
        struct ct_alloc_entry* entry = &ct_alloc_table[i];
        if (entry->state != CT_ENTRY_USED && entry->state != CT_ENTRY_FREED &&
            entry->state != CT_ENTRY_AUTOFREED)
            continue;

        if (!entry->ptr || entry->size == 0)
            continue;

        uintptr_t base = reinterpret_cast<uintptr_t>(entry->ptr);
        if (addr >= base && (addr - base) < entry->size)
        {
            if (base_out)
            {
                *base_out = entry->ptr;
            }
            if (size_out)
            {
                *size_out = entry->size;
            }
            if (req_size_out)
            {
                *req_size_out = entry->req_size;
            }
            if (site_out)
            {
                *site_out = entry->site;
            }
            if (state_out)
            {
                *state_out = entry->state;
            }
            return 1;
        }
    }

    return 0;
}

CT_NODISCARD CT_NOINSTR static size_t ct_malloc_usable_size(void* ptr, size_t fallback)
{
    if (!ptr)
        return 0;

#if defined(__APPLE__)
    return malloc_size(ptr);
#elif defined(__GLIBC__) || defined(__linux__)
    return malloc_usable_size(ptr);
#else
    return fallback;
#endif
}

CT_NOINSTR static void ct_shadow_track_alloc(void* ptr, size_t req_size, size_t real_size)
{
    if (!ct_shadow_enabled || !ptr)
        return;

    ct_shadow_unpoison_range(ptr, req_size);
    uintptr_t start = reinterpret_cast<uintptr_t>(ptr) + req_size;
    uintptr_t end = reinterpret_cast<uintptr_t>(ptr) + real_size;
    uintptr_t poison_start = (start + 7u) & ~static_cast<uintptr_t>(7u);
    if (poison_start < end)
    {
        ct_shadow_poison_range(reinterpret_cast<void*>(poison_start),
                               static_cast<size_t>(end - poison_start));
    }
}

CT_NOINSTR static void ct_log_alloc_details(const char* label, const char* status, size_t req_size,
                                            size_t real_size, void* ptr, const char* site,
                                            CTColor color, CTLevel lvl)
{
    ct_log(lvl, "{}{}{} :: tid={} site={}\n", ct_color(color), label, ct_color(CTColor::Reset),
           ct_thread_id(), ct_site_name(site));
    ct_log(lvl, "┌-----------------------------------┐\n");
    ct_log(lvl, "| {:<16} : {:<14} |\n", "status", status);
    ct_log(lvl, "| {:<16} : {:<14} |\n", "req_size", req_size);
    ct_log(lvl, "| {:<16} : {:<14} |\n", "total_alloc_size", real_size);
    ct_log(lvl, "| {:<16} : {:<14} |\n", "ptr", std::format("{:p}", ptr));
    ct_log(lvl, "└-----------------------------------┘\n");
}

CT_NOINSTR static void ct_log_realloc_details(const char* label, const char* status,
                                              size_t old_req_size, size_t old_real_size,
                                              void* old_ptr, size_t new_req_size,
                                              size_t new_real_size, void* new_ptr, const char* site,
                                              CTColor color)
{
    ct_log(CTLevel::Warn, "{}{}{} :: tid={} site={}\n", ct_color(color), label,
           ct_color(CTColor::Reset), ct_thread_id(), ct_site_name(site));
    ct_log(CTLevel::Warn, "┌-----------------------------------┐\n");
    ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "status", status);
    ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "old_req_size", old_req_size);
    ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "new_req_size", new_req_size);
    ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "old_alloc_size", old_real_size);
    ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "new_alloc_size", new_real_size);
    ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "old_ptr", std::format("{:p}", old_ptr));
    ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "new_ptr", std::format("{:p}", new_ptr));
    ct_log(CTLevel::Warn, "└-----------------------------------┘\n");
}

CT_NODISCARD CT_NOINSTR static void* ct_malloc_impl(size_t size, const char* site, int unreachable)
{
    ct_init_env_once();
    if (ct_disable_alloc)
        return malloc(size);

    void* ptr = malloc(size);
    size_t real_size = ct_malloc_usable_size(ptr, size);

    ct_lock_acquire();
    if (ptr && !ct_table_insert(ptr, size, real_size, site, CT_ALLOC_KIND_MALLOC))
    {
        if (!ct_alloc_table_full_logged)
        {
            ct_alloc_table_full_logged = 1;
            ct_log(CTLevel::Warn, "{}alloc table full ({} entries){}\n", ct_color(CTColor::Red),
                   ct_alloc_table_size, ct_color(CTColor::Reset));
        }
    }
    ct_lock_release();

    ct_shadow_track_alloc(ptr, size, real_size);

    if (unreachable)
    {
        if (ct_alloc_trace_enabled)
        {
            ct_log_alloc_details("tracing-malloc-unreachable", "unreachable", size, real_size, ptr,
                                 site, CTColor::Yellow, CTLevel::Warn);
        }
        if (ptr && ct_autofree_enabled)
        {
            __ct_autofree(ptr);
        }
    }
    if (!unreachable)
    {
        if (ct_alloc_trace_enabled)
        {
            ct_log_alloc_details("tracing-malloc", "reachable", size, real_size, ptr, site,
                                 CTColor::Yellow, CTLevel::Info);
        }
    }

    return ptr;
}

CT_NODISCARD CT_NOINSTR static void* ct_calloc_impl(size_t count, size_t size, const char* site,
                                                    int unreachable)
{
    ct_init_env_once();
    if (ct_disable_alloc)
        return calloc(count, size);

    size_t req_size = 0;
    bool overflow = __builtin_mul_overflow(count, size, &req_size);
    if (overflow)
        req_size = 0;

    void* ptr = calloc(count, size);
    size_t real_size = ct_malloc_usable_size(ptr, req_size);
    size_t shadow_size = overflow ? real_size : req_size;

    ct_lock_acquire();
    if (ptr && !ct_table_insert(ptr, req_size, real_size, site, CT_ALLOC_KIND_MALLOC))
    {
        if (!ct_alloc_table_full_logged)
        {
            ct_alloc_table_full_logged = 1;
            ct_log(CTLevel::Warn, "{}alloc table full ({} entries){}\n", ct_color(CTColor::Red),
                   ct_alloc_table_size, ct_color(CTColor::Reset));
        }
    }
    ct_lock_release();

    ct_shadow_track_alloc(ptr, shadow_size, real_size);

    if (unreachable)
    {
        if (ct_alloc_trace_enabled)
        {
            ct_log_alloc_details("tracing-calloc-unreachable", "unreachable", req_size, real_size,
                                 ptr, site, CTColor::Yellow, CTLevel::Warn);
        }
        if (ptr && ct_autofree_enabled)
        {
            __ct_autofree(ptr);
        }
    }
    else
    {
        if (ct_alloc_trace_enabled)
        {
            ct_log_alloc_details("tracing-calloc", "reachable", req_size, real_size, ptr, site,
                                 CTColor::Yellow, CTLevel::Info);
        }
    }

    return ptr;
}

CT_NODISCARD CT_NOINSTR static void* ct_new_impl(size_t size, const char* site, int unreachable,
                                                 int is_array)
{
    ct_init_env_once();
    if (ct_disable_alloc)
        return is_array ? ::operator new[](size) : ::operator new(size);

    void* ptr = is_array ? ::operator new[](size) : ::operator new(size);
    size_t real_size = ct_malloc_usable_size(ptr, size);

    ct_lock_acquire();
    unsigned char kind = is_array ? CT_ALLOC_KIND_NEW_ARRAY : CT_ALLOC_KIND_NEW;
    if (ptr && !ct_table_insert(ptr, size, real_size, site, kind))
    {
        if (!ct_alloc_table_full_logged)
        {
            ct_alloc_table_full_logged = 1;
            ct_log(CTLevel::Warn, "{}alloc table full ({} entries){}\n", ct_color(CTColor::Red),
                   ct_alloc_table_size, ct_color(CTColor::Reset));
        }
    }
    ct_lock_release();

    ct_shadow_track_alloc(ptr, size, real_size);

    const char* label = is_array ? "tracing-new-array" : "tracing-new";
    const char* label_unreachable =
        is_array ? "tracing-new-array-unreachable" : "tracing-new-unreachable";

    if (unreachable)
    {
        if (ct_alloc_trace_enabled)
        {
            ct_log_alloc_details(label_unreachable, "unreachable", size, real_size, ptr, site,
                                 CTColor::Yellow, CTLevel::Warn);
        }
        if (ptr && ct_autofree_enabled)
        {
            __ct_autofree(ptr);
        }
    }
    else
    {
        if (ct_alloc_trace_enabled)
        {
            ct_log_alloc_details(label, "reachable", size, real_size, ptr, site, CTColor::Yellow,
                                 CTLevel::Info);
        }
    }

    return ptr;
}

CT_NODISCARD CT_NOINSTR static void* ct_new_nothrow_impl(size_t size, const char* site,
                                                         int unreachable, int is_array)
{
    ct_init_env_once();
    if (ct_disable_alloc)
        return is_array ? ::operator new[](size, std::nothrow)
                         : ::operator new(size, std::nothrow);
    void* ptr = is_array ? ::operator new[](size, std::nothrow)
                         : ::operator new(size, std::nothrow);
    if (!ptr)
        return nullptr;

    size_t real_size = ct_malloc_usable_size(ptr, size);

    unsigned char kind = is_array ? CT_ALLOC_KIND_NEW_ARRAY : CT_ALLOC_KIND_NEW;
    ct_lock_acquire();
    if (ptr && !ct_table_insert(ptr, size, real_size, site, kind))
    {
        if (!ct_alloc_table_full_logged)
        {
            ct_alloc_table_full_logged = 1;
            ct_log(CTLevel::Warn, "{}alloc table full ({} entries){}\n", ct_color(CTColor::Red),
                   ct_alloc_table_size, ct_color(CTColor::Reset));
        }
    }
    ct_lock_release();

    ct_shadow_track_alloc(ptr, size, real_size);

    const char* label = is_array ? "tracing-new-array" : "tracing-new";
    const char* label_unreachable =
        is_array ? "tracing-new-array-unreachable" : "tracing-new-unreachable";

    if (unreachable)
    {
        if (ct_alloc_trace_enabled)
        {
            ct_log_alloc_details(label_unreachable, "unreachable", size, real_size, ptr, site,
                                 CTColor::Yellow, CTLevel::Warn);
        }
        if (ptr && ct_autofree_enabled)
        {
            __ct_autofree(ptr);
        }
    }
    else
    {
        if (ct_alloc_trace_enabled)
        {
            ct_log_alloc_details(label, "reachable", size, real_size, ptr, site, CTColor::Yellow,
                                 CTLevel::Info);
        }
    }

    return ptr;
}

CT_NODISCARD CT_NOINSTR static void* ct_realloc_impl(void* ptr, size_t size, const char* site)
{
    ct_init_env_once();
    if (ct_disable_alloc)
        return realloc(ptr, size);

    size_t old_size = 0;
    size_t old_req_size = 0;
    int had_entry = 0;

    ct_lock_acquire();
    if (ptr)
        had_entry = ct_table_lookup(ptr, &old_size, &old_req_size, nullptr, nullptr);

    ct_lock_release();

    void* new_ptr = realloc(ptr, size);
    if (!new_ptr && size > 0)
    {
        if (ct_alloc_trace_enabled)
        {
            ct_log_realloc_details("tracing-realloc", "failed", old_req_size, old_size, ptr, size,
                                   0, nullptr, site, CTColor::Yellow);
        }
        return nullptr;
    }

    size_t real_size = ct_malloc_usable_size(new_ptr, size);

    ct_lock_acquire();
    if (new_ptr)
    {
        if (ptr && new_ptr != ptr)
            (void)ct_table_remove(ptr, nullptr, nullptr, nullptr);

        if (!ct_table_insert(new_ptr, size, real_size, site, CT_ALLOC_KIND_MALLOC))
        {
            if (!ct_alloc_table_full_logged)
            {
                ct_alloc_table_full_logged = 1;
                ct_log(CTLevel::Warn, "{}alloc table full ({} entries){}\n", ct_color(CTColor::Red),
                       ct_alloc_table_size, ct_color(CTColor::Reset));
            }
        }
    }
    else if (ptr && size == 0)
    {
        (void)ct_table_remove(ptr, nullptr, nullptr, nullptr);
    }
    ct_lock_release();

    if (ct_shadow_enabled)
    {
        if (ptr && new_ptr != ptr && had_entry && old_size)
        {
            ct_shadow_poison_range(ptr, old_size);
        }
        if (new_ptr)
        {
            ct_shadow_track_alloc(new_ptr, size, real_size);
        }
        else if (ptr && size == 0 && had_entry && old_size)
        {
            ct_shadow_poison_range(ptr, old_size);
        }
    }

    if (ct_alloc_trace_enabled)
    {
        const char* status = "updated";
        if (size == 0 && ptr)
        {
            status = "freed";
        }
        else if (!ptr && new_ptr)
        {
            status = "allocated";
        }
        else if (new_ptr == ptr)
        {
            status = "in-place";
        }
        else if (new_ptr)
        {
            status = "moved";
        }

        ct_log_realloc_details("tracing-realloc", status, old_req_size, old_size, ptr, size,
                               real_size, new_ptr, site, CTColor::Yellow);
    }

    return new_ptr;
}

CT_NOINSTR static void ct_delete_impl(void* ptr, int is_array)
{
    ct_init_env_once();
    if (ct_disable_alloc)
    {
        if (is_array)
        {
            ::operator delete[](ptr);
        }
        else
        {
            ::operator delete(ptr);
        }
        return;
    }

    size_t size = 0;
    size_t req_size = 0;
    const char* site = nullptr;
    int found = 0;
    (void)req_size;

    ct_lock_acquire();
    if (ptr)
        found = ct_table_remove(ptr, &size, &req_size, &site);

    ct_lock_release();

    const char* label = is_array ? "tracing-delete-array" : "tracing-delete";

    if (!ptr)
    {
        ct_log(CTLevel::Warn, "{}{} ptr=null{}\n", ct_color(CTColor::Yellow), label,
               ct_color(CTColor::Reset));
        if (is_array)
        {
            ::operator delete[](ptr);
        }
        else
        {
            ::operator delete(ptr);
        }
        return;
    }
    if (found == -1)
    {
        ct_log(CTLevel::Warn, "{}{} ptr={:p} (double free){}\n", ct_color(CTColor::Red), label, ptr,
               ct_color(CTColor::Reset));
        return;
    }
    if (found == 0)
    {
        ct_log(CTLevel::Warn, "{}{} ptr={:p} (unknown){}\n", ct_color(CTColor::Red), label, ptr,
               ct_color(CTColor::Reset));
        if (is_array)
        {
            ::operator delete[](ptr);
        }
        else
        {
            ::operator delete(ptr);
        }
        return;
    }

    if (ct_shadow_enabled)
    {
        ct_shadow_poison_range(ptr, size);
    }

    if (ct_alloc_trace_enabled)
    {
        ct_log(CTLevel::Info, "{}{} ptr={:p} size={}{}\n", ct_color(CTColor::Cyan), label, ptr,
               size, ct_color(CTColor::Reset));
    }

    if (is_array)
    {
        ::operator delete[](ptr);
    }
    else
    {
        ::operator delete(ptr);
    }
}

CT_NOINSTR static void ct_delete_nothrow_impl(void* ptr, int is_array)
{
    ct_init_env_once();
    if (ct_disable_alloc)
    {
        if (is_array)
        {
            ::operator delete[](ptr, std::nothrow);
        }
        else
        {
            ::operator delete(ptr, std::nothrow);
        }
        return;
    }

    size_t size = 0;
    size_t req_size = 0;
    const char* site = nullptr;
    int found = 0;
    (void)req_size;

    ct_lock_acquire();
    if (ptr)
        found = ct_table_remove(ptr, &size, &req_size, &site);

    ct_lock_release();

    const char* label = is_array ? "tracing-delete-array" : "tracing-delete";

    if (!ptr)
    {
        ct_log(CTLevel::Warn, "{}{} ptr=null{}\n", ct_color(CTColor::Yellow), label,
               ct_color(CTColor::Reset));
        if (is_array)
        {
            ::operator delete[](ptr, std::nothrow);
        }
        else
        {
            ::operator delete(ptr, std::nothrow);
        }
        return;
    }
    if (found == -1)
    {
        ct_log(CTLevel::Warn, "{}{} ptr={:p} (double free){}\n", ct_color(CTColor::Red), label, ptr,
               ct_color(CTColor::Reset));
        return;
    }
    if (found == 0)
    {
        ct_log(CTLevel::Warn, "{}{} ptr={:p} (unknown){}\n", ct_color(CTColor::Red), label, ptr,
               ct_color(CTColor::Reset));
        if (is_array)
        {
            ::operator delete[](ptr, std::nothrow);
        }
        else
        {
            ::operator delete(ptr, std::nothrow);
        }
        return;
    }

    if (ct_shadow_enabled)
    {
        ct_shadow_poison_range(ptr, size);
    }

    if (ct_alloc_trace_enabled)
    {
        ct_log(CTLevel::Info, "{}{} ptr={:p} size={}{}\n", ct_color(CTColor::Cyan), label, ptr,
               size, ct_color(CTColor::Reset));
    }

    if (is_array)
    {
        ::operator delete[](ptr, std::nothrow);
    }
    else
    {
        ::operator delete(ptr, std::nothrow);
    }
}

CT_NOINSTR static void ct_delete_destroying_impl(void* ptr, int is_array)
{
    ct_init_env_once();
    if (ct_disable_alloc)
    {
        if (is_array)
        {
            ::operator delete[](ptr);
        }
        else
        {
            ::operator delete(ptr);
        }
        return;
    }

    size_t size = 0;
    size_t req_size = 0;
    const char* site = nullptr;
    int found = 0;
    (void)req_size;

    ct_lock_acquire();
    if (ptr)
        found = ct_table_remove(ptr, &size, &req_size, &site);

    ct_lock_release();

    const char* label = is_array ? "tracing-delete-array" : "tracing-delete";

    if (!ptr)
    {
        ct_log(CTLevel::Warn, "{}{} ptr=null{}\n", ct_color(CTColor::Yellow), label,
               ct_color(CTColor::Reset));
        if (is_array)
        {
            ::operator delete[](ptr);
        }
        else
        {
            ::operator delete(ptr);
        }
        return;
    }
    if (found == -1)
    {
        ct_log(CTLevel::Warn, "{}{} ptr={:p} (double free){}\n", ct_color(CTColor::Red), label, ptr,
               ct_color(CTColor::Reset));
        return;
    }
    if (found == 0)
    {
        ct_log(CTLevel::Warn, "{}{} ptr={:p} (unknown){}\n", ct_color(CTColor::Red), label, ptr,
               ct_color(CTColor::Reset));
        if (is_array)
        {
            ::operator delete[](ptr);
        }
        else
        {
            ::operator delete(ptr);
        }
        return;
    }

    if (ct_shadow_enabled)
    {
        ct_shadow_poison_range(ptr, size);
    }

    if (ct_alloc_trace_enabled)
    {
        ct_log(CTLevel::Info, "{}{} ptr={:p} size={}{}\n", ct_color(CTColor::Cyan), label, ptr,
               size, ct_color(CTColor::Reset));
    }

    if (is_array)
    {
        ::operator delete[](ptr);
    }
    else
    {
        ::operator delete(ptr);
    }
}

extern "C"
{

    CT_NODISCARD CT_NOINSTR void* __ct_malloc(size_t size, const char* site)
    {
        return ct_malloc_impl(size, site, 0);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_malloc_unreachable(size_t size, const char* site)
    {
        return ct_malloc_impl(size, site, 1);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_calloc(size_t count, size_t size, const char* site)
    {
        return ct_calloc_impl(count, size, site, 0);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_calloc_unreachable(size_t count, size_t size,
                                                          const char* site)
    {
        return ct_calloc_impl(count, size, site, 1);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new(size_t size, const char* site)
    {
        return ct_new_impl(size, site, 0, 0);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_unreachable(size_t size, const char* site)
    {
        return ct_new_impl(size, site, 1, 0);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_array(size_t size, const char* site)
    {
        return ct_new_impl(size, site, 0, 1);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_array_unreachable(size_t size, const char* site)
    {
        return ct_new_impl(size, site, 1, 1);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_nothrow(size_t size, const char* site)
    {
        return ct_new_nothrow_impl(size, site, 0, 0);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_nothrow_unreachable(size_t size, const char* site)
    {
        return ct_new_nothrow_impl(size, site, 1, 0);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_array_nothrow(size_t size, const char* site)
    {
        return ct_new_nothrow_impl(size, site, 0, 1);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_array_nothrow_unreachable(size_t size, const char* site)
    {
        return ct_new_nothrow_impl(size, site, 1, 1);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_realloc(void* ptr, size_t size, const char* site)
    {
        return ct_realloc_impl(ptr, size, site);
    }

    CT_NODISCARD CT_NOINSTR int __ct_posix_memalign(void** out, size_t align, size_t size,
                                                    const char* site)
    {
        ct_init_env_once();
        if (ct_disable_alloc)
        {
            return posix_memalign(out, align, size);
        }
        int rc = posix_memalign(out, align, size);
        if (rc != 0 || !out || !*out)
        {
            return rc;
        }

        void* ptr = *out;
        size_t real_size = ct_malloc_usable_size(ptr, size);

        ct_lock_acquire();
        if (!ct_table_insert(ptr, size, real_size, site, CT_ALLOC_KIND_MALLOC))
        {
            if (!ct_alloc_table_full_logged)
            {
                ct_alloc_table_full_logged = 1;
                ct_log(CTLevel::Warn, "{}alloc table full ({} entries){}\n", ct_color(CTColor::Red),
                       ct_alloc_table_size, ct_color(CTColor::Reset));
            }
        }
        ct_lock_release();

        ct_shadow_track_alloc(ptr, size, real_size);

        if (ct_alloc_trace_enabled)
        {
            ct_log_alloc_details("tracing-posix-memalign", "reachable", size, real_size, ptr, site,
                                 CTColor::Yellow, CTLevel::Info);
        }

        return rc;
    }

    CT_NODISCARD CT_NOINSTR void* __ct_aligned_alloc(size_t align, size_t size, const char* site)
    {
        ct_init_env_once();
        if (ct_disable_alloc)
        {
            return aligned_alloc(align, size);
        }
        void* ptr = aligned_alloc(align, size);
        size_t real_size = ct_malloc_usable_size(ptr, size);

        ct_lock_acquire();
        if (ptr && !ct_table_insert(ptr, size, real_size, site, CT_ALLOC_KIND_MALLOC))
        {
            if (!ct_alloc_table_full_logged)
            {
                ct_alloc_table_full_logged = 1;
                ct_log(CTLevel::Warn, "{}alloc table full ({} entries){}\n", ct_color(CTColor::Red),
                       ct_alloc_table_size, ct_color(CTColor::Reset));
            }
        }
        ct_lock_release();

        ct_shadow_track_alloc(ptr, size, real_size);

        if (ct_alloc_trace_enabled)
        {
            ct_log_alloc_details("tracing-aligned-alloc", "reachable", size, real_size, ptr, site,
                                 CTColor::Yellow, CTLevel::Info);
        }

        return ptr;
    }

    CT_NODISCARD CT_NOINSTR void* __ct_mmap(void* addr, size_t len, int prot, int flags, int fd,
                                            size_t offset, const char* site)
    {
        ct_init_env_once();
        void* ptr = mmap(addr, len, prot, flags, fd, static_cast<off_t>(offset));
        if (ptr == MAP_FAILED)
        {
            return ptr;
        }

        ct_lock_acquire();
        if (!ct_table_insert(ptr, len, len, site, CT_ALLOC_KIND_MMAP))
        {
            if (!ct_alloc_table_full_logged)
            {
                ct_alloc_table_full_logged = 1;
                ct_log(CTLevel::Warn, "{}alloc table full ({} entries){}\n", ct_color(CTColor::Red),
                       ct_alloc_table_size, ct_color(CTColor::Reset));
            }
        }
        ct_lock_release();

        ct_shadow_track_alloc(ptr, len, len);

        if (ct_alloc_trace_enabled)
        {
            ct_log_alloc_details("tracing-mmap", "reachable", len, len, ptr, site, CTColor::Yellow,
                                 CTLevel::Info);
        }

        return ptr;
    }

    CT_NODISCARD CT_NOINSTR int __ct_munmap(void* addr, size_t len, const char* site)
    {
        ct_init_env_once();
        size_t size = 0;
        size_t req_size = 0;
        const char* alloc_site = nullptr;
        int found = 0;
        (void)req_size;

        ct_lock_acquire();
        if (addr)
        {
            found = ct_table_remove(addr, &size, &req_size, &alloc_site);
        }
        ct_lock_release();

        if (ct_shadow_enabled && found > 0)
        {
            ct_shadow_poison_range(addr, size);
        }

        if (ct_alloc_trace_enabled)
        {
            ct_log(CTLevel::Info, "{}tracing-munmap ptr={:p} size={}{}\n",
                   ct_color(CTColor::Cyan), addr, len, ct_color(CTColor::Reset));
        }

        return munmap(addr, len);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_sbrk(size_t incr, const char* site)
    {
        ct_init_env_once();
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wdeprecated-declarations"

        void* prev = sbrk(static_cast<intptr_t>(incr));
        #pragma clang diagnostic pop
        if (prev == (void*)-1 || incr == 0)
        {
            return prev;
        }

        if (static_cast<intptr_t>(incr) > 0)
        {
            ct_lock_acquire();
            if (!ct_table_insert(prev, incr, incr, site, CT_ALLOC_KIND_SBRK))
            {
                if (!ct_alloc_table_full_logged)
                {
                    ct_alloc_table_full_logged = 1;
                    ct_log(CTLevel::Warn, "{}alloc table full ({} entries){}\n",
                           ct_color(CTColor::Red), ct_alloc_table_size, ct_color(CTColor::Reset));
                }
            }
            ct_lock_release();

            if (ct_shadow_enabled)
            {
                ct_shadow_track_alloc(prev, incr, incr);
            }
            if (ct_alloc_trace_enabled)
            {
                ct_log_alloc_details("tracing-sbrk", "reachable", incr, incr, prev, site,
                                     CTColor::Yellow, CTLevel::Info);
            }
        }
        else
        {
            void* new_break = static_cast<char*>(prev) + static_cast<intptr_t>(incr);
            size_t size = 0;
            size_t req_size = 0;
            const char* alloc_site = nullptr;
            ct_lock_acquire();
            (void)ct_table_remove(new_break, &size, &req_size, &alloc_site);
            ct_lock_release();
            if (ct_shadow_enabled && size)
            {
                ct_shadow_poison_range(new_break, size);
            }
        }

        return prev;
    }

    CT_NODISCARD CT_NOINSTR void* __ct_brk(void* addr, const char* site)
    {
        ct_init_env_once();

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

        void* rc = brk(addr);
#pragma clang diagnostic pop
        if (ct_alloc_trace_enabled)
        {
            ct_log(CTLevel::Info, "{}tracing-brk addr={:p} rc={:p} site={}{}\n",
                   ct_color(CTColor::Cyan), addr, rc, ct_site_name(site),
                   ct_color(CTColor::Reset));
        }
        return rc;
    }

    CT_NOINSTR void __ct_autofree(void* ptr)
    {
        ct_init_env_once();
        ct_autofree_scan_init_once();
        if (ct_disable_alloc)
        {
            return;
        }
        if (!ct_autofree_enabled)
        {
            return;
        }
        if (!ptr)
        {
            ct_log(CTLevel::Warn, "{}ct: auto-free ptr=null{}\n", ct_color(CTColor::BgBrightYellow),
                   ct_color(CTColor::Reset));
            return;
        }

        size_t size = 0;
        size_t req_size = 0;
        const char* site = nullptr;
        int found = 0;
        (void)req_size;

        if (ct_autofree_scan_enabled && ct_autofree_scan_ptr)
        {
            unsigned char state = CT_ENTRY_EMPTY;
            ct_lock_acquire();
            int lookup = ct_table_lookup(ptr, &size, &req_size, &site, &state);
            ct_lock_release();
            if (lookup == 1 && state == CT_ENTRY_USED)
            {
                if (ct_autofree_scan_for_ptr(ptr, size))
                {
                    return;
                }
            }
        }

        ct_lock_acquire();
        found = ct_table_remove_autofree(ptr, &size, &req_size, &site);
        ct_lock_release();

        if (found == -2)
        {
            return;
        }
        if (found == -1)
        {
            ct_log(CTLevel::Warn, "{}ct: auto-free skipped ptr={:p} (already freed){}\n",
                   ct_color(CTColor::BgBrightYellow), ptr, ct_color(CTColor::Reset));
            return;
        }
        if (found == 0)
        {
            ct_log(CTLevel::Warn, "{}ct: auto-free skipped ptr={:p} (unknown){}\n",
                   ct_color(CTColor::BgBrightYellow), ptr, ct_color(CTColor::Reset));
            return;
        }

        if (ct_shadow_enabled)
        {
            ct_shadow_poison_range(ptr, size);
        }

        ct_log(CTLevel::Warn, "{}auto-free ptr={:p} size={} site={}{}\n",
               ct_color(CTColor::BgBrightYellow), ptr, size, ct_site_name(site),
               ct_color(CTColor::Reset));
        free(ptr);
    }

    CT_NOINSTR void __ct_autofree_munmap(void* ptr)
    {
        ct_init_env_once();
        ct_autofree_scan_init_once();
        if (ct_disable_alloc || !ct_autofree_enabled)
        {
            return;
        }
        if (!ptr)
        {
            ct_log(CTLevel::Warn, "{}ct: auto-free ptr=null{}\n", ct_color(CTColor::BgBrightYellow),
                   ct_color(CTColor::Reset));
            return;
        }

        size_t size = 0;
        size_t req_size = 0;
        const char* site = nullptr;
        int found = 0;
        (void)req_size;

        if (ct_autofree_scan_enabled && ct_autofree_scan_ptr)
        {
            unsigned char state = CT_ENTRY_EMPTY;
            ct_lock_acquire();
            int lookup = ct_table_lookup(ptr, &size, &req_size, &site, &state);
            ct_lock_release();
            if (lookup == 1 && state == CT_ENTRY_USED)
            {
                if (ct_autofree_scan_for_ptr(ptr, size))
                {
                    return;
                }
            }
        }

        ct_lock_acquire();
        found = ct_table_remove_autofree(ptr, &size, &req_size, &site);
        ct_lock_release();

        if (found == -2)
        {
            return;
        }
        if (found <= 0)
        {
            ct_log(CTLevel::Warn, "{}ct: auto-free skipped ptr={:p} ({}){}\n",
                   ct_color(CTColor::BgBrightYellow), ptr,
                   found == 0 ? "unknown" : "already freed", ct_color(CTColor::Reset));
            return;
        }

        if (ct_shadow_enabled)
        {
            ct_shadow_poison_range(ptr, size);
        }

        ct_log(CTLevel::Warn, "{}auto-free ptr={:p} size={} site={}{}\n",
               ct_color(CTColor::BgBrightYellow), ptr, size, ct_site_name(site),
               ct_color(CTColor::Reset));
        (void)munmap(ptr, size);
    }

    CT_NOINSTR void __ct_autofree_sbrk(void* ptr)
    {
        ct_init_env_once();
        ct_autofree_scan_init_once();
        if (ct_disable_alloc || !ct_autofree_enabled)
        {
            return;
        }
        if (!ptr)
        {
            ct_log(CTLevel::Warn, "{}ct: auto-free ptr=null{}\n", ct_color(CTColor::BgBrightYellow),
                   ct_color(CTColor::Reset));
            return;
        }

        size_t size = 0;
        size_t req_size = 0;
        const char* site = nullptr;
        int found = 0;
        (void)req_size;

        if (ct_autofree_scan_enabled && ct_autofree_scan_ptr)
        {
            unsigned char state = CT_ENTRY_EMPTY;
            ct_lock_acquire();
            int lookup = ct_table_lookup(ptr, &size, &req_size, &site, &state);
            ct_lock_release();
            if (lookup == 1 && state == CT_ENTRY_USED)
            {
                if (ct_autofree_scan_for_ptr(ptr, size))
                {
                    return;
                }
            }
        }

        ct_lock_acquire();
        found = ct_table_remove_autofree(ptr, &size, &req_size, &site);
        ct_lock_release();

        if (found == -2)
        {
            return;
        }
        if (found <= 0)
        {
            ct_log(CTLevel::Warn, "{}ct: auto-free skipped ptr={:p} ({}){}\n",
                   ct_color(CTColor::BgBrightYellow), ptr,
                   found == 0 ? "unknown" : "already freed", ct_color(CTColor::Reset));
            return;
        }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

        void* current = sbrk(0);
        if (current != (void*)-1 &&
        static_cast<char*>(ptr) + static_cast<ptrdiff_t>(size) == current)
        {
            (void)sbrk(-static_cast<intptr_t>(size));
            if (ct_shadow_enabled)
            {
                ct_shadow_poison_range(ptr, size);
            }
            ct_log(CTLevel::Warn, "{}auto-free ptr={:p} size={} site={}{}\n",
                ct_color(CTColor::BgBrightYellow), ptr, size, ct_site_name(site),
                ct_color(CTColor::Reset));
                return;
#pragma clang diagnostic pop
        }

        ct_log(CTLevel::Warn, "{}ct: auto-free skipped ptr={:p} (sbrk not top){}\n",
               ct_color(CTColor::BgBrightYellow), ptr, ct_color(CTColor::Reset));
    }

    CT_NOINSTR void __ct_autofree_delete(void* ptr)
    {
        ct_init_env_once();
        ct_autofree_scan_init_once();
        if (ct_disable_alloc || !ct_autofree_enabled)
        {
            return;
        }
        if (!ptr)
        {
            ct_log(CTLevel::Warn, "{}ct: auto-free ptr=null{}\n", ct_color(CTColor::BgBrightYellow),
                   ct_color(CTColor::Reset));
            return;
        }

        size_t size = 0;
        size_t req_size = 0;
        const char* site = nullptr;
        int found = 0;
        (void)req_size;

        if (ct_autofree_scan_enabled && ct_autofree_scan_ptr)
        {
            unsigned char state = CT_ENTRY_EMPTY;
            ct_lock_acquire();
            int lookup = ct_table_lookup(ptr, &size, &req_size, &site, &state);
            ct_lock_release();
            if (lookup == 1 && state == CT_ENTRY_USED)
            {
                if (ct_autofree_scan_for_ptr(ptr, size))
                {
                    return;
                }
            }
        }

        ct_lock_acquire();
        found = ct_table_remove_autofree(ptr, &size, &req_size, &site);
        ct_lock_release();

        if (found == -2)
        {
            return;
        }
        if (found <= 0)
        {
            ct_log(CTLevel::Warn, "{}ct: auto-free skipped ptr={:p} ({}){}\n",
                   ct_color(CTColor::BgBrightYellow), ptr,
                   found == 0 ? "unknown" : "already freed", ct_color(CTColor::Reset));
            return;
        }

        if (ct_shadow_enabled)
        {
            ct_shadow_poison_range(ptr, size);
        }

        ct_log(CTLevel::Warn, "{}auto-free ptr={:p} size={} site={}{}\n",
               ct_color(CTColor::BgBrightYellow), ptr, size, ct_site_name(site),
               ct_color(CTColor::Reset));
        ::operator delete(ptr);
    }

    CT_NOINSTR void __ct_autofree_delete_array(void* ptr)
    {
        ct_init_env_once();
        ct_autofree_scan_init_once();
        if (ct_disable_alloc || !ct_autofree_enabled)
        {
            return;
        }
        if (!ptr)
        {
            ct_log(CTLevel::Warn, "{}ct: auto-free ptr=null{}\n", ct_color(CTColor::BgBrightYellow),
                   ct_color(CTColor::Reset));
            return;
        }

        size_t size = 0;
        size_t req_size = 0;
        const char* site = nullptr;
        int found = 0;
        (void)req_size;

        if (ct_autofree_scan_enabled && ct_autofree_scan_ptr)
        {
            unsigned char state = CT_ENTRY_EMPTY;
            ct_lock_acquire();
            int lookup = ct_table_lookup(ptr, &size, &req_size, &site, &state);
            ct_lock_release();
            if (lookup == 1 && state == CT_ENTRY_USED)
            {
                if (ct_autofree_scan_for_ptr(ptr, size))
                {
                    return;
                }
            }
        }

        ct_lock_acquire();
        found = ct_table_remove_autofree(ptr, &size, &req_size, &site);
        ct_lock_release();

        if (found == -2)
        {
            return;
        }
        if (found <= 0)
        {
            ct_log(CTLevel::Warn, "{}ct: auto-free skipped ptr={:p} ({}){}\n",
                   ct_color(CTColor::BgBrightYellow), ptr,
                   found == 0 ? "unknown" : "already freed", ct_color(CTColor::Reset));
            return;
        }

        if (ct_shadow_enabled)
        {
            ct_shadow_poison_range(ptr, size);
        }

        ct_log(CTLevel::Warn, "{}auto-free ptr={:p} size={} site={}{}\n",
               ct_color(CTColor::BgBrightYellow), ptr, size, ct_site_name(site),
               ct_color(CTColor::Reset));
        ::operator delete[](ptr);
    }

    CT_NOINSTR void __ct_free(void* ptr)
    {
        ct_init_env_once();
        if (ct_disable_alloc)
        {
            free(ptr);
            return;
        }

        size_t size = 0;
        size_t req_size = 0;
        const char* site = nullptr;
        int found = 0;
        (void)req_size;

        ct_lock_acquire();
        if (ptr)
        {
            found = ct_table_remove(ptr, &size, &req_size, &site);
        }
        ct_lock_release();

        if (!ptr)
        {
            ct_log(CTLevel::Warn, "{}tracing-free ptr=null{}\n", ct_color(CTColor::Yellow),
                   ct_color(CTColor::Reset));
            free(ptr);
            return;
        }
        if (found == -1)
        {
            ct_log(CTLevel::Warn, "{}tracing-free ptr={:p} (double free){}\n",
                   ct_color(CTColor::Red), ptr, ct_color(CTColor::Reset));
            return;
        }
        if (found == 0)
        {
            ct_log(CTLevel::Warn, "{}tracing-free ptr={:p} (unknown){}\n", ct_color(CTColor::Red),
                   ptr, ct_color(CTColor::Reset));
            free(ptr);
            return;
        }

        if (ct_shadow_enabled)
        {
            ct_shadow_poison_range(ptr, size);
        }

        if (ct_alloc_trace_enabled)
        {
            ct_log(CTLevel::Info, "{}tracing-free ptr={:p} size={}{}\n", ct_color(CTColor::Cyan),
                   ptr, size, ct_color(CTColor::Reset));
        }
        free(ptr);
    }

    CT_NOINSTR void __ct_delete(void* ptr)
    {
        ct_delete_impl(ptr, 0);
    }

    CT_NOINSTR void __ct_delete_array(void* ptr)
    {
        ct_delete_impl(ptr, 1);
    }

    CT_NOINSTR void __ct_delete_nothrow(void* ptr)
    {
        ct_delete_nothrow_impl(ptr, 0);
    }

    CT_NOINSTR void __ct_delete_array_nothrow(void* ptr)
    {
        ct_delete_nothrow_impl(ptr, 1);
    }

    CT_NOINSTR void __ct_delete_destroying(void* ptr)
    {
#if defined(__cpp_lib_destroying_delete) && __cpp_lib_destroying_delete >= 201806L
        ct_delete_destroying_impl(ptr, 0);
#else
        ct_delete_impl(ptr, 0);
#endif
    }

    CT_NOINSTR void __ct_delete_array_destroying(void* ptr)
    {
#if defined(__cpp_lib_destroying_delete) && __cpp_lib_destroying_delete >= 201806L
        ct_delete_destroying_impl(ptr, 1);
#else
        ct_delete_impl(ptr, 1);
#endif
    }

} // extern "C"

CT_NOINSTR __attribute__((destructor)) static void ct_report_leaks(void)
{
    if (ct_alloc_count == 0)
        return;

    ct_disable_logging();

    ct_write_prefix(CTLevel::Error);
    ct_write_str(ct_color(CTColor::Red));
    ct_write_cstr("ct: leaks detected count=");
    ct_write_dec(ct_alloc_count);
    ct_write_str(ct_color(CTColor::Reset));
    ct_write_cstr("\n");

    size_t reported = 0;
    for (size_t i = 0; i < ct_alloc_table_size; ++i)
    {
        if (ct_alloc_table[i].state != CT_ENTRY_USED)
            continue;

        ct_write_prefix(CTLevel::Warn);
        ct_write_str(ct_color(CTColor::Yellow));
        ct_write_cstr("ct: leak ptr=");
        ct_write_hex(reinterpret_cast<uintptr_t>(ct_alloc_table[i].ptr));
        ct_write_cstr(" size=");
        ct_write_dec(ct_alloc_table[i].size);
        ct_write_str(ct_color(CTColor::Reset));
        ct_write_cstr("\n");

        if (++reported >= 32)
        {
            ct_write_prefix(CTLevel::Warn);
            ct_write_str(ct_color(CTColor::Yellow));
            ct_write_cstr("ct: leak list truncated");
            ct_write_str(ct_color(CTColor::Reset));
            ct_write_cstr("\n");
            break;
        }
    }
}
