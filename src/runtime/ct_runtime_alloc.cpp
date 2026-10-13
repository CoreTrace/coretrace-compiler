// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_alloc_internal.h"
#include "ct_runtime_block_index.h"
#include "ct_runtime_quarantine.h"

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

// The initial storage of the table and of its index (ct_alloc_index), side by side: both hold
// the address of every block they record, and the conservative scan skips this one range.
static struct
{
    struct ct_alloc_entry table[CT_ALLOC_TABLE_SIZE];
    alignas(uintptr_t) unsigned char index[ct_block_index_storage_size(CT_ALLOC_TABLE_SIZE)];
} ct_alloc_storage;
struct ct_alloc_entry* ct_alloc_table = ct_alloc_storage.table;
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
    ct_spin_lock_acquire(&ct_alloc_lock);
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

CT_NOINSTR void ct_alloc_table_storage_bounds(uintptr_t* begin, uintptr_t* end)
{
    *begin = reinterpret_cast<uintptr_t>(&ct_alloc_storage);
    *end = reinterpret_cast<uintptr_t>(&ct_alloc_storage + 1);
}

// The index of the blocks the table records, live or freed, by address, for
// ct_table_lookup_containing (#185). It holds a block while its record is one the lookup sees,
// and changes with the record, under ct_alloc_lock. It starts in ct_alloc_storage, at the
// table's initial capacity.
static ct_block_index ct_alloc_index_state;
static bool ct_alloc_index_ready = false;

CT_NODISCARD CT_NOINSTR static ct_block_index* ct_alloc_index(void)
{
    if (!ct_alloc_index_ready)
    {
        ct_block_index_init(&ct_alloc_index_state, ct_alloc_storage.index, CT_ALLOC_TABLE_SIZE);
        ct_alloc_index_ready = true;
    }
    return &ct_alloc_index_state;
}

// A record the containing lookup sees: a live or freed block, of some size.
CT_NODISCARD CT_NOINSTR static bool ct_entry_is_indexed(const struct ct_alloc_entry* entry)
{
    return (entry->state == CT_ENTRY_USED || entry->state == CT_ENTRY_FREED ||
            entry->state == CT_ENTRY_AUTOFREED) &&
           entry->ptr && entry->size != 0;
}

// Called before a record changes and after it, so that the index follows it.
CT_NOINSTR static void ct_unindex_entry(const struct ct_alloc_entry* entry)
{
    if (ct_entry_is_indexed(entry))
    {
        ct_block_index_remove(ct_alloc_index(), reinterpret_cast<uintptr_t>(entry->ptr),
                              entry->size);
    }
}

CT_NOINSTR static void ct_index_entry(const struct ct_alloc_entry* entry)
{
    if (ct_entry_is_indexed(entry))
    {
        // On failure, the index is marked incomplete and the lookup scans the table.
        (void)ct_block_index_add(ct_alloc_index(), reinterpret_cast<uintptr_t>(entry->ptr),
                                 entry->size);
    }
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

    if (ct_alloc_table != ct_alloc_storage.table)
        std::free(ct_alloc_table);

    ct_alloc_table = new_table;
    ct_alloc_table_bits = new_bits;
    ct_alloc_table_size = new_size;
    ct_alloc_table_mask = new_mask;
    ct_alloc_count = new_count;
    ct_alloc_table_full_logged = 0;

    return 1;
}

// Takes `entry` for a live block. ct_alloc_lock held.
CT_NOINSTR static void ct_claim_entry(struct ct_alloc_entry* entry, void* ptr, size_t req_size,
                                      size_t size, const char* site, unsigned char kind)
{
    // The record may hold a freed block, of this address or another one.
    ct_unindex_entry(entry);
    entry->ptr = ptr;
    entry->size = size;
    entry->req_size = req_size;
    entry->site = site;
    entry->kind = kind;
    entry->mark = 0;
    entry->state = CT_ENTRY_USED;
    ++ct_alloc_count;
    ct_index_entry(entry);
}

CT_NODISCARD CT_NOINSTR int ct_table_insert(void* ptr, size_t req_size, size_t size,
                                            const char* site, unsigned char kind)
{
    constexpr size_t kNone = static_cast<size_t>(-1);
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        size_t idx = ct_hash_ptr(ptr, ct_alloc_table_mask);
        size_t tombstone = kNone;
        size_t freed = kNone;

        for (size_t i = 0; i < ct_alloc_table_size; ++i)
        {
            size_t pos = (idx + i) & ct_alloc_table_mask;
            struct ct_alloc_entry* entry = &ct_alloc_table[pos];

            if (entry->state == CT_ENTRY_USED)
            {
                if (entry->ptr == ptr)
                {
                    ct_unindex_entry(entry);
                    entry->size = size;
                    entry->req_size = req_size;
                    entry->site = site;
                    entry->kind = kind;
                    entry->mark = 0;
                    ct_index_entry(entry);
                    return 1;
                }
                continue;
            }

            // A block recorded as freed is in the quarantine, still allocated: its record
            // must stay, unless this very address is being handed out again.
            if (entry->state == CT_ENTRY_FREED || entry->state == CT_ENTRY_AUTOFREED)
            {
                if (entry->ptr == ptr)
                {
                    ct_claim_entry(entry, ptr, req_size, size, site, kind);
                    return 1;
                }
                if (freed == kNone)
                {
                    freed = pos;
                }
                continue;
            }

            if (entry->state == CT_ENTRY_TOMB)
            {
                if (tombstone == kNone)
                {
                    tombstone = pos;
                }
                continue;
            }

            // CT_ENTRY_EMPTY: the end of the probe sequence.
            ct_claim_entry(tombstone != kNone ? &ct_alloc_table[tombstone] : entry, ptr, req_size,
                           size, site, kind);
            return 1;
        }

        if (tombstone != kNone)
        {
            ct_claim_entry(&ct_alloc_table[tombstone], ptr, req_size, size, site, kind);
            return 1;
        }

        if (!ct_alloc_grow_locked())
        {
            // The table cannot grow: a freed record gives way to the live block, whose
            // tracking matters more.
            if (freed != kNone)
            {
                ct_claim_entry(&ct_alloc_table[freed], ptr, req_size, size, site, kind);
                return 1;
            }
            return 0;
        }
    }

    return 0;
}

// ct_table_remove, which also gives the kind of the allocation.
CT_NODISCARD CT_NOINSTR static int ct_table_remove_with_kind(void* ptr, size_t* size_out,
                                                             size_t* req_size_out,
                                                             const char** site_out,
                                                             unsigned char* kind_out)
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
            if (kind_out)
            {
                *kind_out = entry->kind;
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
            if (kind_out)
            {
                *kind_out = entry->kind;
            }
            return -1;
        }
    }

    return 0;
}

CT_NODISCARD CT_NOINSTR int ct_table_remove(void* ptr, size_t* size_out, size_t* req_size_out,
                                            const char** site_out)
{
    return ct_table_remove_with_kind(ptr, size_out, req_size_out, site_out, nullptr);
}

// The kind of an allocation, as releases name it.
CT_NODISCARD CT_NOINSTR static const char* ct_alloc_kind_name(unsigned char kind)
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
    case CT_ALLOC_KIND_OBJC:
        return "objc";
    default:
        return "unknown";
    }
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

// ct_table_lookup_containing over every record, for when the index misses blocks.
CT_NODISCARD CT_NOINSTR static int ct_table_scan_containing(const void* ptr, void** base_out,
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

CT_NODISCARD CT_NOINSTR int ct_table_lookup_containing(const void* ptr, void** base_out,
                                                       size_t* size_out, size_t* req_size_out,
                                                       const char** site_out,
                                                       unsigned char* state_out)
{
    if (!ptr)
        return 0;

    const ct_block_index* index = ct_alloc_index();
    if (index->incomplete)
    {
        return ct_table_scan_containing(ptr, base_out, size_out, req_size_out, site_out, state_out);
    }

    uintptr_t base = 0;
    size_t size = 0;
    if (!ct_block_index_find(index, reinterpret_cast<uintptr_t>(ptr), &base, &size))
        return 0;

    const struct ct_alloc_entry* entry = ct_table_find_entry(reinterpret_cast<void*>(base));
    if (!entry)
        return 0;

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
        ct_unindex_entry(entry);
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
    unsigned char old_state = CT_ENTRY_EMPTY;

    ct_lock_acquire();
    if (ptr)
        (void)ct_table_lookup(ptr, &old_size, &old_req_size, nullptr, &old_state);

    ct_lock_release();

    // A block recorded as freed is in the quarantine, which will release it: the C library
    // must not release it first.
    if (old_state == CT_ENTRY_FREED || old_state == CT_ENTRY_AUTOFREED)
    {
        ct_log_realloc_freed(ptr, site);
        return nullptr;
    }

    // A tracked block is never resized in place, nor released by the C library: the
    // contents move to a new block and the old one goes to the quarantine, so that a
    // pointer still aiming at it reads a block recorded as freed. realloc(ptr, 0) releases
    // it the same way and returns NULL. Other blocks, which the runtime keeps no record of,
    // go through the C library's realloc.
    const bool tracked = old_state == CT_ENTRY_USED;
    void* new_ptr = nullptr;
    if (tracked && size > 0)
    {
        new_ptr = malloc(size);
        if (new_ptr)
        {
            std::memcpy(new_ptr, ptr, old_req_size < size ? old_req_size : size);
        }
    }
    else if (!tracked)
    {
        new_ptr = realloc(ptr, size);
    }
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
    if (tracked)
    {
        (void)ct_table_remove(ptr, nullptr, nullptr, nullptr);
    }
    if (new_ptr && !ct_table_insert(new_ptr, size, real_size, site, CT_ALLOC_KIND_MALLOC))
    {
        ct_warn_alloc_table_full();
    }
    ct_lock_release();

    if (ct_is_enabled(CT_FEATURE_SHADOW))
    {
        if (tracked && old_size)
        {
            ct_shadow_poison_range(ptr, old_size);
        }
        if (new_ptr)
        {
            ct_shadow_track_alloc(new_ptr, size, real_size);
        }
    }
    if (tracked)
    {
        ct_quarantine_push(
            {ptr, old_size, static_cast<unsigned char>(CtReleaseApi::Free), CT_ALLOC_KIND_MALLOC});
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
CT_NODISCARD CT_NOINSTR static int ct_release_api_is_array(CtReleaseApi api)
{
    return api == CtReleaseApi::DeleteArray || api == CtReleaseApi::DeleteArrayNothrow ||
           api == CtReleaseApi::DeleteArrayDestroying;
}

CT_NOINSTR static void ct_release_by_api(void* ptr, CtReleaseApi api)
{
    switch (api)
    {
    case CtReleaseApi::Free:
        free(ptr);
        return;
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

// The allocator's side of the quarantine, see ct_runtime_quarantine.h.
CT_NOINSTR void ct_table_forget_freed(void* ptr)
{
    struct ct_alloc_entry* entry = ct_table_find_entry(ptr);
    if (entry && (entry->state == CT_ENTRY_FREED || entry->state == CT_ENTRY_AUTOFREED))
    {
        ct_unindex_entry(entry);
        entry->state = CT_ENTRY_TOMB;
    }
}

CT_NOINSTR void ct_forget_returned_block(void* ptr, size_t size)
{
    ct_lock_acquire();
    ct_table_forget_freed(ptr);
    ct_lock_release();
    ct_shadow_unpoison_range(ptr, size);
}

CT_NOINSTR void ct_quarantine_release(const ct_quarantine_item& item)
{
    ct_shadow_unpoison_range(item.ptr, item.size);
    ct_release_by_api(item.ptr, static_cast<CtReleaseApi>(item.api));
}

CT_NOINSTR static void ct_release_tracked_pointer(void* ptr, CtReleaseApi api, const char* site)
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
    const char* alloc_site = nullptr;
    unsigned char recorded_kind = CT_ALLOC_KIND_NEW;
    int found = 0;
    (void)req_size;

    ct_lock_acquire();
    if (ptr)
        found = ct_table_remove_with_kind(ptr, &size, &req_size, &alloc_site, &recorded_kind);

    ct_lock_release();

    const char* label = is_array ? "tracing-delete-array" : "tracing-delete";

    if (!ptr)
    {
        ct_log_release_null(label);
        ct_release_by_api(ptr, api);
        return;
    }
    if (found == -1)
    {
        ct_log_double_free(label, ptr, site, alloc_site);
        return;
    }
    if (found == 0)
    {
        // A block the runtime did not track, such as one the C++ library allocated: its
        // release is traced like any other, not reported.
        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            ct_log_release_unknown(label, ptr);
        }
        ct_release_by_api(ptr, api);
        return;
    }

    if (ct_is_enabled(CT_FEATURE_SHADOW))
    {
        ct_shadow_poison_range(ptr, size);
    }

    if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
    {
        ct_log_release(label, ptr, size, ct_alloc_kind_name(recorded_kind), alloc_site);
    }

    const unsigned char kind =
        ct_release_api_is_array(api) ? CT_ALLOC_KIND_NEW_ARRAY : CT_ALLOC_KIND_NEW;
    ct_quarantine_push({ptr, size, static_cast<unsigned char>(api), kind});
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
        unsigned char kind = CT_ALLOC_KIND_MMAP;
        int found = 0;
        (void)req_size;

        ct_lock_acquire();
        if (addr)
        {
            found = ct_table_remove_with_kind(addr, &size, &req_size, &alloc_site, &kind);
        }
        ct_lock_release();

        if (found > 0)
        {
            ct_forget_returned_block(addr, size);
        }

        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            if (found > 0)
            {
                ct_log_release("tracing-munmap", addr, len, ct_alloc_kind_name(kind), alloc_site);
            }
            else
            {
                ct_log_release_unknown("tracing-munmap", addr);
            }
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
            const int found = ct_table_remove(new_break, &size, &req_size, &alloc_site);
            ct_lock_release();
            if (found > 0)
            {
                ct_forget_returned_block(new_break, size);
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
                ct_forget_returned_block(ptr, size);
                ct_log_autofree(ptr, size, site);
                return;
            }
#pragma clang diagnostic pop
            ct_log_autofree_skipped(ptr, "sbrk not top");
#else
            (void)size;
            ct_log_autofree_skipped(ptr, "sbrk not supported");
#endif
            return;
        }

        ct_log_autofree(ptr, size, site);

        if (api == CtAutoFreeApi::Munmap)
        {
            ct_forget_returned_block(ptr, size);
            (void)munmap(ptr, size);
            return;
        }

        if (ct_is_enabled(CT_FEATURE_SHADOW))
        {
            ct_shadow_poison_range(ptr, size);
        }
        switch (api)
        {
        case CtAutoFreeApi::Delete:
            ct_quarantine_push(
                {ptr, size, static_cast<unsigned char>(CtReleaseApi::Delete), CT_ALLOC_KIND_NEW});
            return;
        case CtAutoFreeApi::DeleteArray:
            ct_quarantine_push({ptr, size, static_cast<unsigned char>(CtReleaseApi::DeleteArray),
                                CT_ALLOC_KIND_NEW_ARRAY});
            return;
        case CtAutoFreeApi::Free:
        case CtAutoFreeApi::Sbrk:
        case CtAutoFreeApi::Munmap:
            ct_quarantine_push(
                {ptr, size, static_cast<unsigned char>(CtReleaseApi::Free), CT_ALLOC_KIND_MALLOC});
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
            ct_log_autofree_skipped(ptr, nullptr);
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
            ct_log_autofree_skipped(ptr, found == 0 ? "unknown" : "already freed");
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

    CT_NOINSTR void __ct_free(void* ptr, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            free(ptr);
            return;
        }

        size_t size = 0;
        size_t req_size = 0;
        const char* alloc_site = nullptr;
        unsigned char kind = CT_ALLOC_KIND_MALLOC;
        int found = 0;
        (void)req_size;

        ct_lock_acquire();
        if (ptr)
        {
            found = ct_table_remove_with_kind(ptr, &size, &req_size, &alloc_site, &kind);
        }
        ct_lock_release();

        if (!ptr)
        {
            ct_log_release_null("tracing-free");
            free(ptr);
            return;
        }
        if (found == -1)
        {
            ct_log_double_free("tracing-free", ptr, site, alloc_site);
            return;
        }
        if (found == 0)
        {
            // A block the runtime did not track, such as one libc allocated: its release is
            // traced like any other, not reported.
            if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
            {
                ct_log_release_unknown("tracing-free", ptr);
            }
            free(ptr);
            return;
        }

        if (ct_is_enabled(CT_FEATURE_SHADOW))
        {
            ct_shadow_poison_range(ptr, size);
        }

        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            ct_log_release("tracing-free", ptr, size, ct_alloc_kind_name(kind), alloc_site);
        }
        ct_quarantine_push(
            {ptr, size, static_cast<unsigned char>(CtReleaseApi::Free), CT_ALLOC_KIND_MALLOC});
    }

    CT_NOINSTR void __ct_delete(void* ptr, const char* site)
    {
        ct_release_tracked_pointer(ptr, CtReleaseApi::Delete, site);
    }

    CT_NOINSTR void __ct_delete_array(void* ptr, const char* site)
    {
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteArray, site);
    }

    CT_NOINSTR void __ct_delete_nothrow(void* ptr, const char* site)
    {
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteNothrow, site);
    }

    CT_NOINSTR void __ct_delete_array_nothrow(void* ptr, const char* site)
    {
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteArrayNothrow, site);
    }

    CT_NOINSTR void __ct_delete_destroying(void* ptr, const char* site)
    {
#if defined(__cpp_lib_destroying_delete) && __cpp_lib_destroying_delete >= 201806L
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteDestroying, site);
#else
        ct_release_tracked_pointer(ptr, CtReleaseApi::Delete, site);
#endif
    }

    CT_NOINSTR void __ct_delete_array_destroying(void* ptr, const char* site)
    {
#if defined(__cpp_lib_destroying_delete) && __cpp_lib_destroying_delete >= 201806L
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteArrayDestroying, site);
#else
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteArray, site);
#endif
    }

} // extern "C"

// The report must run after the program's own exit-time code, which may release blocks:
// exit handlers, destructors of global objects, destructor functions.
CT_NOINSTR static void ct_report_leaks(void)
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

#if defined(__APPLE__)
extern "C" void __cxa_finalize(void* dso);

// Mach-O keeps one list of exit-time code, run in reverse order of registration: exit
// handlers, destructors of global objects, and destructor functions, which clang registers
// the same way, those of dylibs included. Dylibs and objects initialized before the report
// was scheduled registered theirs earlier, so they would run after it (#158). The report
// therefore runs the rest of the list first: __cxa_finalize marks each entry as run before
// calling it, so every handler still runs once, in the same order.
CT_NOINSTR static void ct_report_leaks_last(void)
{
    __cxa_finalize(nullptr);
    ct_report_leaks();
}

// The report registered first runs last: every module that tracks allocations schedules it
// from a constructor that runs before its static initializers.
extern "C" CT_NOINSTR void __ct_schedule_leak_report(void)
{
    static int scheduled = 0;
    if (__atomic_exchange_n(&scheduled, 1, __ATOMIC_ACQ_REL) == 0)
    {
        std::atexit(ct_report_leaks_last);
    }
}

// Objects compiled without that constructor still get a report, scheduled when the runtime
// starts: after the static initializers of the objects linked before it.
CT_NOINSTR __attribute__((constructor)) static void ct_schedule_leak_report_late(void)
{
    __ct_schedule_leak_report();
}
#else
// ELF runs .fini_array after every exit handler, the destructors of global objects among
// them, and the lowest priority last: 101, the lowest a program may use, puts this function
// after every destructor function of default priority. The executable's .fini_array runs
// before those of the shared libraries it depends on, whose exit-time code may still release
// tracked blocks (#158). glibc finalizes every object from one exit handler, and runs the
// handlers registered meanwhile once it returns: registered from here, the report runs after
// all of them. Where registration is refused, the report runs here.
extern "C" CT_NOINSTR void __ct_schedule_leak_report(void) {}

CT_NOINSTR __attribute__((destructor(101))) static void ct_report_leaks_at_exit(void)
{
    if (std::atexit(ct_report_leaks) != 0)
        ct_report_leaks();
}
#endif
