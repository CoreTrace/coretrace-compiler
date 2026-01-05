#include "ct_runtime_internal.h"

#include <cstdlib>
#include <cstring>
#include <format>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

struct ct_alloc_entry {
    void *ptr;
    size_t size;
    size_t req_size;
    const char *site;
    unsigned char state;
};

#define CT_ALLOC_TABLE_BITS 16u
#define CT_ALLOC_TABLE_SIZE (1u << CT_ALLOC_TABLE_BITS)

static struct ct_alloc_entry ct_alloc_table[CT_ALLOC_TABLE_SIZE];
static size_t ct_alloc_count = 0;
static int ct_alloc_lock = 0;

extern "C" {
CT_NOINSTR void __ct_autofree(void *ptr);
}

CT_NOINSTR void ct_lock_acquire(void)
{
    while (__atomic_exchange_n(&ct_alloc_lock, 1, __ATOMIC_ACQUIRE) != 0) {
    }
}

CT_NOINSTR void ct_lock_release(void)
{
    __atomic_store_n(&ct_alloc_lock, 0, __ATOMIC_RELEASE);
}

CT_NOINSTR static size_t ct_hash_ptr(const void *ptr)
{
    uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
    value ^= value >> 4;
    value ^= value >> 9;
    return static_cast<size_t>(value) & (CT_ALLOC_TABLE_SIZE - 1u);
}

CT_NOINSTR int ct_table_insert(void *ptr, size_t req_size, size_t size, const char *site)
{
    size_t idx = ct_hash_ptr(ptr);
    size_t tombstone = static_cast<size_t>(-1);

    for (size_t i = 0; i < CT_ALLOC_TABLE_SIZE; ++i) {
        size_t pos = (idx + i) & (CT_ALLOC_TABLE_SIZE - 1u);
        struct ct_alloc_entry *entry = &ct_alloc_table[pos];

        if (entry->state == CT_ENTRY_USED) {
            if (entry->ptr == ptr) {
                entry->size = size;
                entry->req_size = req_size;
                entry->site = site;
                return 1;
            }
            continue;
        }

        if ((entry->state == CT_ENTRY_TOMB || entry->state == CT_ENTRY_FREED) &&
            tombstone == static_cast<size_t>(-1)) {
            tombstone = pos;
            continue;
        }

        if (entry->state == CT_ENTRY_EMPTY) {
            if (tombstone != static_cast<size_t>(-1)) {
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

    if (tombstone != static_cast<size_t>(-1)) {
        struct ct_alloc_entry *entry = &ct_alloc_table[tombstone];
        entry->ptr = ptr;
        entry->size = size;
        entry->req_size = req_size;
        entry->site = site;
        entry->state = CT_ENTRY_USED;
        ++ct_alloc_count;
        return 1;
    }

    return 0;
}

CT_NOINSTR int ct_table_remove(void *ptr,
                               size_t *size_out,
                               size_t *req_size_out,
                               const char **site_out)
{
    size_t idx = ct_hash_ptr(ptr);

    for (size_t i = 0; i < CT_ALLOC_TABLE_SIZE; ++i) {
        size_t pos = (idx + i) & (CT_ALLOC_TABLE_SIZE - 1u);
        struct ct_alloc_entry *entry = &ct_alloc_table[pos];

        if (entry->state == CT_ENTRY_EMPTY) {
            return 0;
        }
        if (entry->state == CT_ENTRY_USED && entry->ptr == ptr) {
            if (size_out) {
                *size_out = entry->size;
            }
            if (req_size_out) {
                *req_size_out = entry->req_size;
            }
            if (site_out) {
                *site_out = entry->site;
            }
            if (ct_alloc_count > 0) {
                --ct_alloc_count;
            }
            entry->state = CT_ENTRY_FREED;
            return 1;
        }
        if (entry->state == CT_ENTRY_FREED && entry->ptr == ptr) {
            if (size_out) {
                *size_out = entry->size;
            }
            if (req_size_out) {
                *req_size_out = entry->req_size;
            }
            if (site_out) {
                *site_out = entry->site;
            }
            return -1;
        }
    }

    return 0;
}

CT_NOINSTR int ct_table_lookup(const void *ptr,
                               size_t *size_out,
                               size_t *req_size_out,
                               const char **site_out,
                               unsigned char *state_out)
{
    size_t idx = ct_hash_ptr(ptr);

    for (size_t i = 0; i < CT_ALLOC_TABLE_SIZE; ++i) {
        size_t pos = (idx + i) & (CT_ALLOC_TABLE_SIZE - 1u);
        struct ct_alloc_entry *entry = &ct_alloc_table[pos];

        if (entry->state == CT_ENTRY_EMPTY) {
            return 0;
        }
        if ((entry->state == CT_ENTRY_USED || entry->state == CT_ENTRY_FREED) && entry->ptr == ptr) {
            if (size_out) {
                *size_out = entry->size;
            }
            if (req_size_out) {
                *req_size_out = entry->req_size;
            }
            if (site_out) {
                *site_out = entry->site;
            }
            if (state_out) {
                *state_out = entry->state;
            }
            return 1;
        }
    }

    return 0;
}

CT_NOINSTR int ct_table_lookup_containing(const void *ptr,
                                          void **base_out,
                                          size_t *size_out,
                                          size_t *req_size_out,
                                          const char **site_out,
                                          unsigned char *state_out)
{
    if (!ptr) {
        return 0;
    }

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

    for (size_t i = 0; i < CT_ALLOC_TABLE_SIZE; ++i) {
        struct ct_alloc_entry *entry = &ct_alloc_table[i];
        if (entry->state != CT_ENTRY_USED && entry->state != CT_ENTRY_FREED) {
            continue;
        }
        if (!entry->ptr || entry->size == 0) {
            continue;
        }

        uintptr_t base = reinterpret_cast<uintptr_t>(entry->ptr);
        if (addr >= base && (addr - base) < entry->size) {
            if (base_out) {
                *base_out = entry->ptr;
            }
            if (size_out) {
                *size_out = entry->size;
            }
            if (req_size_out) {
                *req_size_out = entry->req_size;
            }
            if (site_out) {
                *site_out = entry->site;
            }
            if (state_out) {
                *state_out = entry->state;
            }
            return 1;
        }
    }

    return 0;
}

CT_NOINSTR static size_t ct_malloc_usable_size(void *ptr, size_t fallback)
{
    if (!ptr) {
        return 0;
    }
#if defined(__APPLE__)
    return malloc_size(ptr);
#elif defined(__GLIBC__) || defined(__linux__)
    return malloc_usable_size(ptr);
#else
    return fallback;
#endif
}

CT_NOINSTR static void *ct_malloc_impl(size_t size, const char *site, int unreachable)
{
    ct_init_env_once();
    if (ct_disable_alloc) {
        return malloc(size);
    }

    void *ptr = malloc(size);
    size_t real_size = ct_malloc_usable_size(ptr, size);

    ct_lock_acquire();
    if (ptr && !ct_table_insert(ptr, size, real_size, site)) {
        ct_log(CTLevel::Warn,
               "{}alloc table full{}\n",
               ct_color(CTColor::Red),
               ct_color(CTColor::Reset));
    }
    ct_lock_release();

    if (ct_shadow_enabled && ptr) {
        ct_shadow_unpoison_range(ptr, size);
        uintptr_t start = reinterpret_cast<uintptr_t>(ptr) + size;
        uintptr_t end = reinterpret_cast<uintptr_t>(ptr) + real_size;
        uintptr_t poison_start = (start + 7u) & ~static_cast<uintptr_t>(7u);
        if (poison_start < end) {
            ct_shadow_poison_range(reinterpret_cast<void *>(poison_start),
                                   static_cast<size_t>(end - poison_start));
        }
    }

    if (unreachable)
    {
        if (ct_alloc_trace_enabled) {
            ct_log(CTLevel::Warn,
                   "{}tracing-malloc-unreachable{} :: tid={} site={}\n",
                   ct_color(CTColor::Yellow),
                   ct_color(CTColor::Reset),
                   ct_thread_id(),
                   ct_site_name(site));
            ct_log(CTLevel::Warn, "┌-----------------------------------┐\n");
            ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "status", "unreachable");
            ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "req_size", size);
            ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "total_alloc_size", real_size);
            ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "ptr", std::format("{:p}", ptr));
            ct_log(CTLevel::Warn, "└-----------------------------------┘\n");
        }
        if (ptr && ct_autofree_enabled) {
            __ct_autofree(ptr);
        }
    }
    if (!unreachable)
    {
        if (ct_alloc_trace_enabled) {
            ct_log(CTLevel::Warn,
                   "{}tracing-malloc{} :: tid={} site={}\n",
                   ct_color(CTColor::Yellow),
                   ct_color(CTColor::Reset),
                   ct_thread_id(),
                   ct_site_name(site));
            ct_log(CTLevel::Warn, "┌-----------------------------------┐\n");
            ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "status", "reachable");
            ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "req_size", size);
            ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "total_alloc_size", real_size);
            ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "ptr", std::format("{:p}", ptr));
            ct_log(CTLevel::Warn, "└-----------------------------------┘\n");
        }
    }

    return ptr;
}

extern "C" {

CT_NOINSTR void *__ct_malloc(size_t size, const char *site)
{
    return ct_malloc_impl(size, site, 0);
}

CT_NOINSTR void *__ct_malloc_unreachable(size_t size, const char *site)
{
    return ct_malloc_impl(size, site, 1);
}

CT_NOINSTR void __ct_autofree(void *ptr)
{
    ct_init_env_once();
    if (ct_disable_alloc) {
        return;
    }
    if (!ct_autofree_enabled) {
        return;
    }
    if (!ptr) {
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

    if (found == -1) {
        ct_log(CTLevel::Warn,
               "{}ct: auto-free skipped ptr={:p} (already freed){}\n",
               ct_color(CTColor::BgBrightYellow),
               ptr,
               ct_color(CTColor::Reset));
        return;
    }
    if (found == 0) {
        ct_log(CTLevel::Warn,
               "{}ct: auto-free skipped ptr={:p} (unknown){}\n",
               ct_color(CTColor::BgBrightYellow),
               ptr,
               ct_color(CTColor::Reset));
        return;
    }

    if (ct_shadow_enabled) {
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
    if (ct_disable_alloc) {
        free(ptr);
        return;
    }

    size_t size = 0;
    size_t req_size = 0;
    const char *site = nullptr;
    int found = 0;
    (void)req_size;

    ct_lock_acquire();
    if (ptr) {
        found = ct_table_remove(ptr, &size, &req_size, &site);
    }
    ct_lock_release();

    if (!ptr) {
        ct_log(CTLevel::Warn,
               "{}tracing-free ptr=null{}\n",
               ct_color(CTColor::Yellow),
               ct_color(CTColor::Reset));
        free(ptr);
        return;
    }
    if (found == -1) {
        ct_log(CTLevel::Warn,
               "{}tracing-free ptr={:p} (double free){}\n",
               ct_color(CTColor::Red),
               ptr,
               ct_color(CTColor::Reset));
        return;
    }
    if (found == 0) {
        ct_log(CTLevel::Warn,
               "{}tracing-free ptr={:p} (unknown){}\n",
               ct_color(CTColor::Red),
               ptr,
               ct_color(CTColor::Reset));
        free(ptr);
        return;
    }

    if (ct_shadow_enabled) {
        ct_shadow_poison_range(ptr, size);
    }

    if (ct_alloc_trace_enabled) {
        ct_log(CTLevel::Info,
               "{}tracing-free ptr={:p} size={}{}\n",
               ct_color(CTColor::Cyan),
               ptr,
               size,
               ct_color(CTColor::Reset));
    }
    free(ptr);
}

} // extern "C"

CT_NOINSTR __attribute__((destructor))
static void ct_report_leaks(void)
{
    if (ct_alloc_count == 0) {
        return;
    }

    ct_disable_logging();

    ct_write_prefix(CTLevel::Error);
    ct_write_str(ct_color(CTColor::Red));
    ct_write_cstr("ct: leaks detected count=");
    ct_write_dec(ct_alloc_count);
    ct_write_str(ct_color(CTColor::Reset));
    ct_write_cstr("\n");

    size_t reported = 0;
    for (size_t i = 0; i < CT_ALLOC_TABLE_SIZE; ++i) {
        if (ct_alloc_table[i].state != CT_ENTRY_USED) {
            continue;
        }

        ct_write_prefix(CTLevel::Warn);
        ct_write_str(ct_color(CTColor::Yellow));
        ct_write_cstr("ct: leak ptr=");
        ct_write_hex(reinterpret_cast<uintptr_t>(ct_alloc_table[i].ptr));
        ct_write_cstr(" size=");
        ct_write_dec(ct_alloc_table[i].size);
        ct_write_str(ct_color(CTColor::Reset));
        ct_write_cstr("\n");

        if (++reported >= 32) {
            ct_write_prefix(CTLevel::Warn);
            ct_write_str(ct_color(CTColor::Yellow));
            ct_write_cstr("ct: leak list truncated");
            ct_write_str(ct_color(CTColor::Reset));
            ct_write_cstr("\n");
            break;
        }
    }
}
