// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_alloc_internal.h"

#include <cstdlib>
#include <new>
#include <cstring>
#include <format>
#include <new>
#include <chrono>
#include <atomic>
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

#define CT_ALLOC_TABLE_BITS 16u
#define CT_ALLOC_TABLE_MAX_BITS 20u
#define CT_ALLOC_TABLE_SIZE (1u << CT_ALLOC_TABLE_BITS)

static struct ct_alloc_entry ct_alloc_table_storage[CT_ALLOC_TABLE_SIZE];
struct ct_alloc_entry* ct_alloc_table = ct_alloc_table_storage;
static size_t ct_alloc_table_bits = CT_ALLOC_TABLE_BITS;
size_t ct_alloc_table_size = CT_ALLOC_TABLE_SIZE;
size_t ct_alloc_table_mask = CT_ALLOC_TABLE_SIZE - 1u;
size_t ct_alloc_count = 0;
static int ct_alloc_lock = 0;
static int ct_alloc_table_full_logged = 0;

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

// Warns once per process that the allocation table can no longer grow; later
// allocations go untracked. Called with ct_alloc_lock held.
CT_NOINSTR static void ct_warn_alloc_table_full(void)
{
    if (ct_alloc_table_full_logged)
    {
        return;
    }
    ct_alloc_table_full_logged = 1;
    ct_log(CTLevel::Warn, "{}alloc table full ({} entries){}\n", ct_color(CTColor::Red),
           ct_alloc_table_size, ct_color(CTColor::Reset));
}

CT_NODISCARD CT_NOINSTR size_t ct_hash_ptr(const void* ptr, size_t mask)
{
    uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
    value ^= value >> 4;
    value ^= value >> 9;
    return static_cast<size_t>(value) & mask;
}

CT_NODISCARD CT_NOINSTR struct ct_alloc_entry* ct_table_find_entry(const void* ptr)
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
        if (entry->ptr == ptr && (entry->state == CT_ENTRY_USED || entry->state == CT_ENTRY_FREED ||
                                  entry->state == CT_ENTRY_AUTOFREED))
        {
            return entry;
        }
    }
    return nullptr;
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
                                                     size_t* req_size_out, const char** site_out)
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
    if (!ct_is_enabled(CT_FEATURE_SHADOW) || !ptr)
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

CT_NOINSTR void ct_track_allocation(void* ptr, size_t size, const char* site, unsigned char kind)
{
    ct_lock_acquire();
    if (!ct_table_insert(ptr, size, size, site, kind))
    {
        ct_warn_alloc_table_full();
    }
    ct_lock_release();

    ct_shadow_track_alloc(ptr, size, size);
}

CT_NODISCARD CT_NOINSTR size_t ct_forget_allocation(void* ptr, unsigned char kind)
{
    size_t size = 0;
    ct_lock_acquire();
    struct ct_alloc_entry* entry = ct_table_find_entry(ptr);
    if (entry && entry->state == CT_ENTRY_USED && entry->kind == kind)
    {
        size = entry->size;
        entry->state = CT_ENTRY_TOMB;
        --ct_alloc_count;
    }
    ct_lock_release();

    if (size && ct_is_enabled(CT_FEATURE_SHADOW))
    {
        ct_shadow_poison_range(ptr, size);
    }
    return size;
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
    if (!ct_is_enabled(CT_FEATURE_ALLOC))
        return malloc(size);

    void* ptr = malloc(size);
    size_t real_size = ct_malloc_usable_size(ptr, size);

    ct_lock_acquire();
    if (ptr && !ct_table_insert(ptr, size, real_size, site, CT_ALLOC_KIND_MALLOC))
    {
        ct_warn_alloc_table_full();
    }
    ct_lock_release();

    ct_shadow_track_alloc(ptr, size, real_size);

    if (unreachable)
    {
        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            ct_log_alloc_details("tracing-malloc-unreachable", "unreachable", size, real_size, ptr,
                                 site, CTColor::Yellow, CTLevel::Warn);
        }
        if (ptr && ct_is_enabled(CT_FEATURE_AUTOFREE))
        {
            __ct_autofree(ptr);
        }
    }
    if (!unreachable)
    {
        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
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
    if (!ct_is_enabled(CT_FEATURE_ALLOC))
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
        ct_warn_alloc_table_full();
    }
    ct_lock_release();

    ct_shadow_track_alloc(ptr, shadow_size, real_size);

    if (unreachable)
    {
        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            ct_log_alloc_details("tracing-calloc-unreachable", "unreachable", req_size, real_size,
                                 ptr, site, CTColor::Yellow, CTLevel::Warn);
        }
        if (ptr && ct_is_enabled(CT_FEATURE_AUTOFREE))
        {
            __ct_autofree(ptr);
        }
    }
    else
    {
        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
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
    if (!ct_is_enabled(CT_FEATURE_ALLOC))
        return is_array ? ::operator new[](size) : ::operator new(size);

    void* ptr = is_array ? ::operator new[](size) : ::operator new(size);
    size_t real_size = ct_malloc_usable_size(ptr, size);

    ct_lock_acquire();
    unsigned char kind = is_array ? CT_ALLOC_KIND_NEW_ARRAY : CT_ALLOC_KIND_NEW;
    if (ptr && !ct_table_insert(ptr, size, real_size, site, kind))
    {
        ct_warn_alloc_table_full();
    }
    ct_lock_release();

    ct_shadow_track_alloc(ptr, size, real_size);

    const char* label = is_array ? "tracing-new-array" : "tracing-new";
    const char* label_unreachable =
        is_array ? "tracing-new-array-unreachable" : "tracing-new-unreachable";

    if (unreachable)
    {
        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            ct_log_alloc_details(label_unreachable, "unreachable", size, real_size, ptr, site,
                                 CTColor::Yellow, CTLevel::Warn);
        }
        if (ptr && ct_is_enabled(CT_FEATURE_AUTOFREE))
        {
            __ct_autofree(ptr);
        }
    }
    else
    {
        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
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
    if (!ct_is_enabled(CT_FEATURE_ALLOC))
        return is_array ? ::operator new[](size, std::nothrow) : ::operator new(size, std::nothrow);
    void* ptr =
        is_array ? ::operator new[](size, std::nothrow) : ::operator new(size, std::nothrow);
    if (!ptr)
        return nullptr;

    size_t real_size = ct_malloc_usable_size(ptr, size);

    unsigned char kind = is_array ? CT_ALLOC_KIND_NEW_ARRAY : CT_ALLOC_KIND_NEW;
    ct_lock_acquire();
    if (ptr && !ct_table_insert(ptr, size, real_size, site, kind))
    {
        ct_warn_alloc_table_full();
    }
    ct_lock_release();

    ct_shadow_track_alloc(ptr, size, real_size);

    const char* label = is_array ? "tracing-new-array" : "tracing-new";
    const char* label_unreachable =
        is_array ? "tracing-new-array-unreachable" : "tracing-new-unreachable";

    if (unreachable)
    {
        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            ct_log_alloc_details(label_unreachable, "unreachable", size, real_size, ptr, site,
                                 CTColor::Yellow, CTLevel::Warn);
        }
        if (ptr && ct_is_enabled(CT_FEATURE_AUTOFREE))
        {
            __ct_autofree(ptr);
        }
    }
    else
    {
        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
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
    if (!ct_is_enabled(CT_FEATURE_ALLOC))
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
        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
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
            ct_warn_alloc_table_full();
        }
    }
    else if (ptr && size == 0)
    {
        (void)ct_table_remove(ptr, nullptr, nullptr, nullptr);
    }
    ct_lock_release();

    if (ct_is_enabled(CT_FEATURE_SHADOW))
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

    if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
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

// The tracked-release path shared by every operator delete the compiler rewrites.
// Only the deallocation call itself differs between them; everything before it, the
// table removal and the diagnostics, is common.
enum class CtReleaseApi : unsigned char
{
    Delete,
    DeleteArray,
    DeleteNothrow,
    DeleteArrayNothrow,
    DeleteDestroying,
    DeleteArrayDestroying
};

CT_NODISCARD CT_NOINSTR static int ct_release_api_is_array(CtReleaseApi api)
{
    return api == CtReleaseApi::DeleteArray || api == CtReleaseApi::DeleteArrayNothrow ||
           api == CtReleaseApi::DeleteArrayDestroying;
}

CT_NOINSTR static void ct_release_by_api(void* ptr, CtReleaseApi api)
{
    switch (api)
    {
    case CtReleaseApi::DeleteNothrow:
        ::operator delete(ptr, std::nothrow);
        return;
    case CtReleaseApi::DeleteArrayNothrow:
        ::operator delete[](ptr, std::nothrow);
        return;
    case CtReleaseApi::DeleteArray:
    case CtReleaseApi::DeleteArrayDestroying:
        ::operator delete[](ptr);
        return;
    case CtReleaseApi::Delete:
    case CtReleaseApi::DeleteDestroying:
        ::operator delete(ptr);
        return;
    }
}

CT_NOINSTR static void ct_release_tracked_pointer(void* ptr, CtReleaseApi api)
{
    const int is_array = ct_release_api_is_array(api);

    ct_init_env_once();
    if (!ct_is_enabled(CT_FEATURE_ALLOC))
    {
        ct_release_by_api(ptr, api);
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
        ct_release_by_api(ptr, api);
        return;
    }
    if (found == -1)
    {
        ct_log(CTLevel::Warn, "{}{} ptr={:p} (double free) alloc_site={}{}\n",
               ct_color(CTColor::Red), label, ptr, ct_site_name(site), ct_color(CTColor::Reset));
        return;
    }
    if (found == 0)
    {
        ct_log(CTLevel::Warn, "{}{} ptr={:p} (unknown){}\n", ct_color(CTColor::Red), label, ptr,
               ct_color(CTColor::Reset));
        ct_release_by_api(ptr, api);
        return;
    }

    if (ct_is_enabled(CT_FEATURE_SHADOW))
    {
        ct_shadow_poison_range(ptr, size);
    }

    if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
    {
        ct_log(CTLevel::Info, "{}{} ptr={:p} size={}{}\n", ct_color(CTColor::Cyan), label, ptr,
               size, ct_color(CTColor::Reset));
    }

    ct_release_by_api(ptr, api);
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
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
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
            ct_warn_alloc_table_full();
        }
        ct_lock_release();

        ct_shadow_track_alloc(ptr, size, real_size);

        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            ct_log_alloc_details("tracing-posix-memalign", "reachable", size, real_size, ptr, site,
                                 CTColor::Yellow, CTLevel::Info);
        }

        return rc;
    }

    CT_NODISCARD CT_NOINSTR void* __ct_aligned_alloc(size_t align, size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return aligned_alloc(align, size);
        }
        void* ptr = aligned_alloc(align, size);
        size_t real_size = ct_malloc_usable_size(ptr, size);

        ct_lock_acquire();
        if (ptr && !ct_table_insert(ptr, size, real_size, site, CT_ALLOC_KIND_MALLOC))
        {
            ct_warn_alloc_table_full();
        }
        ct_lock_release();

        ct_shadow_track_alloc(ptr, size, real_size);

        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
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
            ct_warn_alloc_table_full();
        }
        ct_lock_release();

        ct_shadow_track_alloc(ptr, len, len);

        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
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

        if (ct_is_enabled(CT_FEATURE_SHADOW) && found > 0)
        {
            ct_shadow_poison_range(addr, size);
        }

        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            ct_log(CTLevel::Info, "{}tracing-munmap ptr={:p} size={}{}\n", ct_color(CTColor::Cyan),
                   addr, len, ct_color(CTColor::Reset));
        }

        return munmap(addr, len);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_sbrk(size_t incr, const char* site)
    {
#if defined(__linux__)
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

            if (ct_is_enabled(CT_FEATURE_SHADOW))
            {
                ct_shadow_track_alloc(prev, incr, incr);
            }
            if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
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
            if (ct_is_enabled(CT_FEATURE_SHADOW) && size)
            {
                ct_shadow_poison_range(new_break, size);
            }
        }

        return prev;
#else
        (void)incr;
        (void)site;
        return reinterpret_cast<void*>(-1);
#endif
    }

    CT_NODISCARD CT_NOINSTR void* __ct_brk(void* addr, const char* site)
    {
#if defined(__linux__)
        ct_init_env_once();

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        int rc = brk(addr);
        void* ret = (rc == 0) ? addr : reinterpret_cast<void*>(-1);
#pragma clang diagnostic pop
        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            ct_log(CTLevel::Info, "{}tracing-brk addr={:p} rc={} ret={:p} site={}{}\n",
                   ct_color(CTColor::Cyan), addr, rc, ret, ct_site_name(site),
                   ct_color(CTColor::Reset));
        }
        return ret;
#else
        (void)addr;
        (void)site;
        return reinterpret_cast<void*>(-1);
#endif
    }

    // Every __ct_autofree* entry point walks the same path: honour the features, give
    // the conservative scan a chance to prove the pointer still reachable, remove the
    // entry, then release it with the API matching how it was allocated.
    enum class CtAutoFreeApi : unsigned char
    {
        Free,
        Munmap,
        Sbrk,
        Delete,
        DeleteArray
    };

    CT_NOINSTR static void ct_autofree_release(void* ptr, size_t size, const char* site,
                                               CtAutoFreeApi api)
    {
        if (api == CtAutoFreeApi::Sbrk)
        {
#if defined(__linux__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

            // The break can only shrink from its top, so an allocation that is no
            // longer the last one cannot be returned to the system.
            void* current = sbrk(0);
            if (current != (void*)-1 &&
                static_cast<char*>(ptr) + static_cast<ptrdiff_t>(size) == current)
            {
                (void)sbrk(-static_cast<intptr_t>(size));
                if (ct_is_enabled(CT_FEATURE_SHADOW))
                {
                    ct_shadow_poison_range(ptr, size);
                }
                ct_log(CTLevel::Warn, "{}auto-free ptr={:p} size={} site={}{}\n",
                       ct_color(CTColor::BgBrightYellow), ptr, size, ct_site_name(site),
                       ct_color(CTColor::Reset));
                return;
            }
#pragma clang diagnostic pop
            ct_log(CTLevel::Warn, "{}ct: auto-free skipped ptr={:p} (sbrk not top){}\n",
                   ct_color(CTColor::BgBrightYellow), ptr, ct_color(CTColor::Reset));
#else
            (void)size;
            ct_log(CTLevel::Warn, "{}ct: auto-free skipped ptr={:p} (sbrk not supported){}\n",
                   ct_color(CTColor::BgBrightYellow), ptr, ct_color(CTColor::Reset));
#endif
            return;
        }

        if (ct_is_enabled(CT_FEATURE_SHADOW))
        {
            ct_shadow_poison_range(ptr, size);
        }

        ct_log(CTLevel::Warn, "{}auto-free ptr={:p} size={} site={}{}\n",
               ct_color(CTColor::BgBrightYellow), ptr, size, ct_site_name(site),
               ct_color(CTColor::Reset));

        switch (api)
        {
        case CtAutoFreeApi::Munmap:
            (void)munmap(ptr, size);
            return;
        case CtAutoFreeApi::Delete:
            ::operator delete(ptr);
            return;
        case CtAutoFreeApi::DeleteArray:
            ::operator delete[](ptr);
            return;
        case CtAutoFreeApi::Free:
        case CtAutoFreeApi::Sbrk:
            free(ptr);
            return;
        }
    }

    CT_NOINSTR static void ct_autofree_tracked(void* ptr, CtAutoFreeApi api)
    {
        ct_init_env_once();
        ct_autofree_scan_init_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC) || !ct_is_enabled(CT_FEATURE_AUTOFREE))
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
        (void)req_size;

        if (ct_autofree_scan_checks_pointers())
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
        const int found = ct_table_remove_autofree(ptr, &size, &req_size, &site);
        ct_lock_release();

        // -2: the scan thread released it already, and said so.
        if (found == -2)
        {
            return;
        }
        if (found <= 0)
        {
            ct_log(CTLevel::Warn, "{}ct: auto-free skipped ptr={:p} ({}){}\n",
                   ct_color(CTColor::BgBrightYellow), ptr, found == 0 ? "unknown" : "already freed",
                   ct_color(CTColor::Reset));
            return;
        }

        ct_autofree_release(ptr, size, site, api);
    }

    CT_NOINSTR void __ct_autofree(void* ptr)
    {
        ct_autofree_tracked(ptr, CtAutoFreeApi::Free);
    }

    CT_NOINSTR void __ct_autofree_munmap(void* ptr)
    {
        ct_autofree_tracked(ptr, CtAutoFreeApi::Munmap);
    }

    CT_NOINSTR void __ct_autofree_sbrk(void* ptr)
    {
        ct_autofree_tracked(ptr, CtAutoFreeApi::Sbrk);
    }

    CT_NOINSTR void __ct_autofree_delete(void* ptr)
    {
        ct_autofree_tracked(ptr, CtAutoFreeApi::Delete);
    }

    CT_NOINSTR void __ct_autofree_delete_array(void* ptr)
    {
        ct_autofree_tracked(ptr, CtAutoFreeApi::DeleteArray);
    }

    CT_NOINSTR void __ct_free(void* ptr)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
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
            ct_log(CTLevel::Warn, "{}tracing-free ptr={:p} (double free) alloc_site={}{}\n",
                   ct_color(CTColor::Red), ptr, ct_site_name(site), ct_color(CTColor::Reset));
            return;
        }
        if (found == 0)
        {
            ct_log(CTLevel::Warn, "{}tracing-free ptr={:p} (unknown){}\n", ct_color(CTColor::Red),
                   ptr, ct_color(CTColor::Reset));
            free(ptr);
            return;
        }

        if (ct_is_enabled(CT_FEATURE_SHADOW))
        {
            ct_shadow_poison_range(ptr, size);
        }

        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            ct_log(CTLevel::Info, "{}tracing-free ptr={:p} size={}{}\n", ct_color(CTColor::Cyan),
                   ptr, size, ct_color(CTColor::Reset));
        }
        free(ptr);
    }

    CT_NOINSTR void __ct_delete(void* ptr)
    {
        ct_release_tracked_pointer(ptr, CtReleaseApi::Delete);
    }

    CT_NOINSTR void __ct_delete_array(void* ptr)
    {
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteArray);
    }

    CT_NOINSTR void __ct_delete_nothrow(void* ptr)
    {
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteNothrow);
    }

    CT_NOINSTR void __ct_delete_array_nothrow(void* ptr)
    {
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteArrayNothrow);
    }

    CT_NOINSTR void __ct_delete_destroying(void* ptr)
    {
#if defined(__cpp_lib_destroying_delete) && __cpp_lib_destroying_delete >= 201806L
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteDestroying);
#else
        ct_release_tracked_pointer(ptr, CtReleaseApi::Delete);
#endif
    }

    CT_NOINSTR void __ct_delete_array_destroying(void* ptr)
    {
#if defined(__cpp_lib_destroying_delete) && __cpp_lib_destroying_delete >= 201806L
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteArrayDestroying);
#else
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteArray);
#endif
    }

} // extern "C"

CT_NOINSTR __attribute__((destructor)) static void ct_report_leaks(void)
{
    // The detached GC thread may still mutate the table; hold the spinlock for the whole
    // report. The ct_write_* primitives write raw bytes and never re-enter the allocator,
    // so they are safe to call under ct_alloc_lock.
    ct_lock_acquire();
    if (ct_alloc_count == 0)
    {
        ct_lock_release();
        return;
    }

    ct_disable_logging();

    ct_write_prefix_nolock(CTLevel::Error);
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

        ct_write_prefix_nolock(CTLevel::Warn);
        ct_write_str(ct_color(CTColor::Yellow));
        ct_write_cstr("ct: leak ptr=");
        ct_write_hex(reinterpret_cast<uintptr_t>(ct_alloc_table[i].ptr));
        ct_write_cstr(" size=");
        ct_write_dec(ct_alloc_table[i].size);
        ct_write_cstr(" alloc_site=");
        ct_write_cstr(ct_site_name(ct_alloc_table[i].site));
        ct_write_str(ct_color(CTColor::Reset));
        ct_write_cstr("\n");

        if (++reported >= 32)
        {
            ct_write_prefix_nolock(CTLevel::Warn);
            ct_write_str(ct_color(CTColor::Yellow));
            ct_write_cstr("ct: leak list truncated");
            ct_write_str(ct_color(CTColor::Reset));
            ct_write_cstr("\n");
            break;
        }
    }
    ct_lock_release();
}
