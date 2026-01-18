#include "ct_runtime_internal.h"

#include <cstdlib>
#include <cstring>
#include <format>
#include <new>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

struct ct_alloc_entry
{
    void *ptr;
    size_t size;
    size_t req_size;
    const char *site;
    unsigned char state;
};

#define CT_ALLOC_TABLE_BITS 16u
#define CT_ALLOC_TABLE_MAX_BITS 20u
#define CT_ALLOC_TABLE_SIZE (1u << CT_ALLOC_TABLE_BITS)

static struct ct_alloc_entry ct_alloc_table_storage[CT_ALLOC_TABLE_SIZE];
static struct ct_alloc_entry *ct_alloc_table = ct_alloc_table_storage;
static size_t ct_alloc_table_bits = CT_ALLOC_TABLE_BITS;
static size_t ct_alloc_table_size = CT_ALLOC_TABLE_SIZE;
static size_t ct_alloc_table_mask = CT_ALLOC_TABLE_SIZE - 1u;
static size_t ct_alloc_count = 0;
static int ct_alloc_lock = 0;
static int ct_alloc_table_full_logged = 0;

extern "C"
{
CT_NOINSTR void __ct_autofree(void *ptr);
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

CT_NODISCARD CT_NOINSTR static size_t ct_hash_ptr(const void *ptr, size_t mask)
{
    uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
    value ^= value >> 4;
    value ^= value >> 9;
    return static_cast<size_t>(value) & mask;
}

CT_NODISCARD CT_NOINSTR static int ct_alloc_rehash_entry(struct ct_alloc_entry *table,
                                            size_t mask,
                                            size_t size,
                                            const struct ct_alloc_entry *entry)
{
    size_t idx = ct_hash_ptr(entry->ptr, mask);
    for (size_t i = 0; i < size; ++i)
    {
        size_t pos = (idx + i) & mask;
        struct ct_alloc_entry *slot = &table[pos];

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
    auto *new_table =
        static_cast<struct ct_alloc_entry *>(
            std::malloc(sizeof(struct ct_alloc_entry) * new_size));
    if (!new_table)
        return 0;

    std::memset(new_table, 0, sizeof(struct ct_alloc_entry) * new_size);

    size_t new_mask = new_size - 1u;
    size_t new_count = 0;
    for (size_t i = 0; i < ct_alloc_table_size; ++i)
    {
        struct ct_alloc_entry *entry = &ct_alloc_table[i];

        if (entry->state != CT_ENTRY_USED && entry->state != CT_ENTRY_FREED)
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

CT_NODISCARD CT_NOINSTR int ct_table_insert(void *ptr, size_t req_size, size_t size, const char *site)
{
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        size_t idx = ct_hash_ptr(ptr, ct_alloc_table_mask);
        size_t tombstone = static_cast<size_t>(-1);

        for (size_t i = 0; i < ct_alloc_table_size; ++i)
        {
            size_t pos = (idx + i) & ct_alloc_table_mask;
            struct ct_alloc_entry *entry = &ct_alloc_table[pos];

            if (entry->state == CT_ENTRY_USED)
            {
                if (entry->ptr == ptr)
                {
                    entry->size = size;
                    entry->req_size = req_size;
                    entry->site = site;
                    return 1;
                }
                continue;
            }

            if ((entry->state == CT_ENTRY_TOMB || entry->state == CT_ENTRY_FREED) &&
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
                entry->state = CT_ENTRY_USED;
                ++ct_alloc_count;
                return 1;
            }
        }

        if (tombstone != static_cast<size_t>(-1))
        {
            struct ct_alloc_entry *entry = &ct_alloc_table[tombstone];
            entry->ptr = ptr;
            entry->size = size;
            entry->req_size = req_size;
            entry->site = site;
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

CT_NODISCARD CT_NOINSTR int ct_table_remove(void *ptr,
                               size_t *size_out,
                               size_t *req_size_out,
                               const char **site_out)
{
    size_t idx = ct_hash_ptr(ptr, ct_alloc_table_mask);

    for (size_t i = 0; i < ct_alloc_table_size; ++i)
    {
        size_t pos = (idx + i) & ct_alloc_table_mask;
        struct ct_alloc_entry *entry = &ct_alloc_table[pos];

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

CT_NODISCARD CT_NOINSTR int ct_table_lookup(const void *ptr,
                               size_t *size_out,
                               size_t *req_size_out,
                               const char **site_out,
                               unsigned char *state_out)
{
    size_t idx = ct_hash_ptr(ptr, ct_alloc_table_mask);

    for (size_t i = 0; i < ct_alloc_table_size; ++i)
    {
        size_t pos = (idx + i) & ct_alloc_table_mask;
        struct ct_alloc_entry *entry = &ct_alloc_table[pos];

        if (entry->state == CT_ENTRY_EMPTY)
        {
            return 0;
        }
        if ((entry->state == CT_ENTRY_USED || entry->state == CT_ENTRY_FREED) && entry->ptr == ptr)
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

CT_NODISCARD CT_NOINSTR int ct_table_lookup_containing(const void *ptr,
                                          void **base_out,
                                          size_t *size_out,
                                          size_t *req_size_out,
                                          const char **site_out,
                                          unsigned char *state_out)
{
    if (!ptr)
        return 0;

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

    for (size_t i = 0; i < ct_alloc_table_size; ++i)
    {
        struct ct_alloc_entry *entry = &ct_alloc_table[i];
        if (entry->state != CT_ENTRY_USED && entry->state != CT_ENTRY_FREED)
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

CT_NODISCARD CT_NOINSTR static size_t ct_malloc_usable_size(void *ptr, size_t fallback)
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

CT_NOINSTR static void ct_shadow_track_alloc(void *ptr, size_t req_size, size_t real_size)
{
    if (!ct_shadow_enabled || !ptr)
        return;

    ct_shadow_unpoison_range(ptr, req_size);
    uintptr_t start = reinterpret_cast<uintptr_t>(ptr) + req_size;
    uintptr_t end = reinterpret_cast<uintptr_t>(ptr) + real_size;
    uintptr_t poison_start = (start + 7u) & ~static_cast<uintptr_t>(7u);
    if (poison_start < end)
    {
        ct_shadow_poison_range(reinterpret_cast<void *>(poison_start),
                               static_cast<size_t>(end - poison_start));
    }
}

CT_NOINSTR static void ct_log_alloc_details(const char *label,
                                            const char *status,
                                            size_t req_size,
                                            size_t real_size,
                                            void *ptr,
                                            const char *site,
                                            CTColor color,
                                            CTLevel lvl)
{
    ct_log(lvl,
           "{}{}{} :: tid={} site={}\n",
           ct_color(color),
           label,
           ct_color(CTColor::Reset),
           ct_thread_id(),
           ct_site_name(site));
    ct_log(lvl, "┌-----------------------------------┐\n");
    ct_log(lvl, "| {:<16} : {:<14} |\n", "status", status);
    ct_log(lvl, "| {:<16} : {:<14} |\n", "req_size", req_size);
    ct_log(lvl, "| {:<16} : {:<14} |\n", "total_alloc_size", real_size);
    ct_log(lvl, "| {:<16} : {:<14} |\n", "ptr", std::format("{:p}", ptr));
    ct_log(lvl, "└-----------------------------------┘\n");
}

CT_NOINSTR static void ct_log_realloc_details(const char *label,
                                              const char *status,
                                              size_t old_req_size,
                                              size_t old_real_size,
                                              void *old_ptr,
                                              size_t new_req_size,
                                              size_t new_real_size,
                                              void *new_ptr,
                                              const char *site,
                                              CTColor color)
{
    ct_log(CTLevel::Warn,
           "{}{}{} :: tid={} site={}\n",
           ct_color(color),
           label,
           ct_color(CTColor::Reset),
           ct_thread_id(),
           ct_site_name(site));
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

CT_NODISCARD CT_NOINSTR static void *ct_malloc_impl(size_t size, const char *site, int unreachable)
{
    ct_init_env_once();
    if (ct_disable_alloc)
        return malloc(size);

    void *ptr = malloc(size);
    size_t real_size = ct_malloc_usable_size(ptr, size);

    ct_lock_acquire();
    if (ptr && !ct_table_insert(ptr, size, real_size, site))
    {
        if (!ct_alloc_table_full_logged)
        {
            ct_alloc_table_full_logged = 1;
            ct_log(CTLevel::Warn,
                   "{}alloc table full ({} entries){}\n",
                   ct_color(CTColor::Red),
                   ct_alloc_table_size,
                   ct_color(CTColor::Reset));
        }
    }
    ct_lock_release();

    ct_shadow_track_alloc(ptr, size, real_size);

    if (unreachable)
    {
        if (ct_alloc_trace_enabled)
        {
            ct_log_alloc_details("tracing-malloc-unreachable",
                                 "unreachable",
                                 size,
                                 real_size,
                                 ptr,
                                 site,
                                 CTColor::Yellow,
                                 CTLevel::Warn);
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
            ct_log_alloc_details("tracing-malloc",
                                 "reachable",
                                 size,
                                 real_size,
                                 ptr,
                                 site,
                                 CTColor::Yellow,
                                 CTLevel::Info);
        }
    }

    return ptr;
}

CT_NODISCARD CT_NOINSTR static void *ct_calloc_impl(size_t count, size_t size, const char *site, int unreachable)
{
    ct_init_env_once();
    if (ct_disable_alloc)
        return calloc(count, size);

    size_t req_size = 0;
    bool overflow = __builtin_mul_overflow(count, size, &req_size);
    if (overflow)
        req_size = 0;

    void *ptr = calloc(count, size);
    size_t real_size = ct_malloc_usable_size(ptr, req_size);
    size_t shadow_size = overflow ? real_size : req_size;

    ct_lock_acquire();
    if (ptr && !ct_table_insert(ptr, req_size, real_size, site))
    {
        if (!ct_alloc_table_full_logged)
        {
            ct_alloc_table_full_logged = 1;
            ct_log(CTLevel::Warn,
                   "{}alloc table full ({} entries){}\n",
                   ct_color(CTColor::Red),
                   ct_alloc_table_size,
                   ct_color(CTColor::Reset));
        }
    }
    ct_lock_release();

    ct_shadow_track_alloc(ptr, shadow_size, real_size);

    if (unreachable)
    {
        if (ct_alloc_trace_enabled)
        {
            ct_log_alloc_details("tracing-calloc-unreachable",
                                 "unreachable",
                                 req_size,
                                 real_size,
                                 ptr,
                                 site,
                                 CTColor::Yellow,
                                 CTLevel::Warn);
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
            ct_log_alloc_details("tracing-calloc",
                                 "reachable",
                                 req_size,
                                 real_size,
                                 ptr,
                                 site,
                                 CTColor::Yellow,
                                 CTLevel::Info);
        }
    }

    return ptr;
}

CT_NODISCARD CT_NOINSTR static void *ct_new_impl(size_t size, const char *site, int unreachable, int is_array)
{
    ct_init_env_once();
    if (ct_disable_alloc)
        return is_array ? ::operator new[](size) : ::operator new(size);


    void *ptr = is_array ? ::operator new[](size) : ::operator new(size);
    size_t real_size = ct_malloc_usable_size(ptr, size);

    ct_lock_acquire();
    if (ptr && !ct_table_insert(ptr, size, real_size, site))
    {
        if (!ct_alloc_table_full_logged)
        {
            ct_alloc_table_full_logged = 1;
            ct_log(CTLevel::Warn,
                   "{}alloc table full ({} entries){}\n",
                   ct_color(CTColor::Red),
                   ct_alloc_table_size,
                   ct_color(CTColor::Reset));
        }
    }
    ct_lock_release();

    ct_shadow_track_alloc(ptr, size, real_size);

    const char *label = is_array ? "tracing-new-array" : "tracing-new";
    const char *label_unreachable =
        is_array ? "tracing-new-array-unreachable" : "tracing-new-unreachable";

    if (unreachable)
    {
        if (ct_alloc_trace_enabled)
        {
            ct_log_alloc_details(label_unreachable,
                                 "unreachable",
                                 size,
                                 real_size,
                                 ptr,
                                 site,
                                 CTColor::Yellow,
                                 CTLevel::Warn);
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
            ct_log_alloc_details(label,
                                 "reachable",
                                 size,
                                 real_size,
                                 ptr,
                                 site,
                                 CTColor::Yellow,
                                 CTLevel::Info);
        }
    }

    return ptr;
}

CT_NODISCARD CT_NOINSTR static void *ct_realloc_impl(void *ptr, size_t size, const char *site)
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

    void *new_ptr = realloc(ptr, size);
    if (!new_ptr && size > 0)
    {
        if (ct_alloc_trace_enabled)
        {
            ct_log_realloc_details("tracing-realloc",
                                   "failed",
                                   old_req_size,
                                   old_size,
                                   ptr,
                                   size,
                                   0,
                                   nullptr,
                                   site,
                                   CTColor::Yellow);
        }
        return nullptr;
    }

    size_t real_size = ct_malloc_usable_size(new_ptr, size);

    ct_lock_acquire();
    if (new_ptr)
    {
        if (ptr && new_ptr != ptr)
            (void)ct_table_remove(ptr, nullptr, nullptr, nullptr);

        if (!ct_table_insert(new_ptr, size, real_size, site))
        {
            if (!ct_alloc_table_full_logged)
            {
                ct_alloc_table_full_logged = 1;
                ct_log(CTLevel::Warn,
                       "{}alloc table full ({} entries){}\n",
                       ct_color(CTColor::Red),
                       ct_alloc_table_size,
                       ct_color(CTColor::Reset));
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
        const char *status = "updated";
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

        ct_log_realloc_details("tracing-realloc",
                               status,
                               old_req_size,
                               old_size,
                               ptr,
                               size,
                               real_size,
                               new_ptr,
                               site,
                               CTColor::Yellow);
    }

    return new_ptr;
}

CT_NOINSTR static void ct_delete_impl(void *ptr, int is_array)
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
    const char *site = nullptr;
    int found = 0;
    (void)req_size;

    ct_lock_acquire();
    if (ptr)
        found = ct_table_remove(ptr, &size, &req_size, &site);

    ct_lock_release();

    const char *label = is_array ? "tracing-delete-array" : "tracing-delete";

    if (!ptr)
    {
        ct_log(CTLevel::Warn,
               "{}{} ptr=null{}\n",
               ct_color(CTColor::Yellow),
               label,
               ct_color(CTColor::Reset));
        if (is_array)
        {
            ::operator delete[](ptr);
        } else {
            ::operator delete(ptr);
        }
        return;
    }
    if (found == -1)
    {
        ct_log(CTLevel::Warn,
               "{}{} ptr={:p} (double free){}\n",
               ct_color(CTColor::Red),
               label,
               ptr,
               ct_color(CTColor::Reset));
        return;
    }
    if (found == 0)
    {
        ct_log(CTLevel::Warn,
               "{}{} ptr={:p} (unknown){}\n",
               ct_color(CTColor::Red),
               label,
               ptr,
               ct_color(CTColor::Reset));
        if (is_array)
        {
            ::operator delete[](ptr);
        } else {
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
        ct_log(CTLevel::Info,
               "{}{} ptr={:p} size={}{}\n",
               ct_color(CTColor::Cyan),
               label,
               ptr,
               size,
               ct_color(CTColor::Reset));
    }

    if (is_array)
    {
        ::operator delete[](ptr);
    } else {
        ::operator delete(ptr);
    }
}

extern "C" {

CT_NODISCARD CT_NOINSTR void *__ct_malloc(size_t size, const char *site)
{
    return ct_malloc_impl(size, site, 0);
}

CT_NODISCARD CT_NOINSTR void *__ct_malloc_unreachable(size_t size, const char *site)
{
    return ct_malloc_impl(size, site, 1);
}

CT_NODISCARD CT_NOINSTR void *__ct_calloc(size_t count, size_t size, const char *site)
{
    return ct_calloc_impl(count, size, site, 0);
}

CT_NODISCARD CT_NOINSTR void *__ct_calloc_unreachable(size_t count, size_t size, const char *site)
{
    return ct_calloc_impl(count, size, site, 1);
}

CT_NODISCARD CT_NOINSTR void *__ct_new(size_t size, const char *site)
{
    return ct_new_impl(size, site, 0, 0);
}

CT_NODISCARD CT_NOINSTR void *__ct_new_unreachable(size_t size, const char *site)
{
    return ct_new_impl(size, site, 1, 0);
}

CT_NODISCARD CT_NOINSTR void *__ct_new_array(size_t size, const char *site)
{
    return ct_new_impl(size, site, 0, 1);
}

CT_NODISCARD CT_NOINSTR void *__ct_new_array_unreachable(size_t size, const char *site)
{
    return ct_new_impl(size, site, 1, 1);
}

CT_NODISCARD CT_NOINSTR void *__ct_realloc(void *ptr, size_t size, const char *site)
{
    return ct_realloc_impl(ptr, size, site);
}

CT_NOINSTR void __ct_autofree(void *ptr)
{
    ct_init_env_once();
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
        ct_log(CTLevel::Warn,
               "{}ct: auto-free ptr=null{}\n",
               ct_color(CTColor::BgBrightYellow),
               ct_color(CTColor::Reset));
        return;
    }

    size_t size = 0;
    size_t req_size = 0;
    const char *site = nullptr;
    int found = 0;
    (void)req_size;

    ct_lock_acquire();
    found = ct_table_remove(ptr, &size, &req_size, &site);
    ct_lock_release();

    if (found == -1)
    {
        ct_log(CTLevel::Warn,
               "{}ct: auto-free skipped ptr={:p} (already freed){}\n",
               ct_color(CTColor::BgBrightYellow),
               ptr,
               ct_color(CTColor::Reset));
        return;
    }
    if (found == 0)
    {
        ct_log(CTLevel::Warn,
               "{}ct: auto-free skipped ptr={:p} (unknown){}\n",
               ct_color(CTColor::BgBrightYellow),
               ptr,
               ct_color(CTColor::Reset));
        return;
    }

    if (ct_shadow_enabled)
    {
        ct_shadow_poison_range(ptr, size);
    }

    ct_log(CTLevel::Warn,
           "{}auto-free ptr={:p} size={} site={}{}\n",
           ct_color(CTColor::BgBrightYellow),
           ptr,
           size,
           ct_site_name(site),
           ct_color(CTColor::Reset));
    free(ptr);
}

CT_NOINSTR void __ct_free(void *ptr)
{
    ct_init_env_once();
    if (ct_disable_alloc)
    {
        free(ptr);
        return;
    }

    size_t size = 0;
    size_t req_size = 0;
    const char *site = nullptr;
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
        ct_log(CTLevel::Warn,
               "{}tracing-free ptr=null{}\n",
               ct_color(CTColor::Yellow),
               ct_color(CTColor::Reset));
        free(ptr);
        return;
    }
    if (found == -1)
    {
        ct_log(CTLevel::Warn,
               "{}tracing-free ptr={:p} (double free){}\n",
               ct_color(CTColor::Red),
               ptr,
               ct_color(CTColor::Reset));
        return;
    }
    if (found == 0)
    {
        ct_log(CTLevel::Warn,
               "{}tracing-free ptr={:p} (unknown){}\n",
               ct_color(CTColor::Red),
               ptr,
               ct_color(CTColor::Reset));
        free(ptr);
        return;
    }

    if (ct_shadow_enabled)
    {
        ct_shadow_poison_range(ptr, size);
    }

    if (ct_alloc_trace_enabled)
    {
        ct_log(CTLevel::Info,
               "{}tracing-free ptr={:p} size={}{}\n",
               ct_color(CTColor::Cyan),
               ptr,
               size,
               ct_color(CTColor::Reset));
    }
    free(ptr);
}

CT_NOINSTR void __ct_delete(void *ptr)
{
    ct_delete_impl(ptr, 0);
}

CT_NOINSTR void __ct_delete_array(void *ptr)
{
    ct_delete_impl(ptr, 1);
}

} // extern "C"

CT_NOINSTR __attribute__((destructor))
static void ct_report_leaks(void)
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
