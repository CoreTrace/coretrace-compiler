#include "ct_runtime_internal.h"

#include <cstdlib>
#include <cstring>

enum {
    CT_SHADOW_ENTRY_EMPTY = 0,
    CT_SHADOW_ENTRY_USED  = 1,
    CT_SHADOW_ENTRY_TOMB  = 2
};

#define CT_SHADOW_SHIFT 3u
#define CT_SHADOW_PAGE_BITS 12u
#define CT_SHADOW_PAGE_SIZE (1u << CT_SHADOW_PAGE_BITS)
#define CT_SHADOW_PAGE_MASK (CT_SHADOW_PAGE_SIZE - 1u)
#define CT_SHADOW_TABLE_BITS 16u
#define CT_SHADOW_TABLE_MAX_BITS 20u
#define CT_SHADOW_TABLE_SIZE (1u << CT_SHADOW_TABLE_BITS)

struct ct_shadow_page_entry {
    uintptr_t page;
    unsigned char *data;
    unsigned char state;
};

static struct ct_shadow_page_entry ct_shadow_table_storage[CT_SHADOW_TABLE_SIZE];
static struct ct_shadow_page_entry *ct_shadow_table = ct_shadow_table_storage;
static size_t ct_shadow_table_bits = CT_SHADOW_TABLE_BITS;
static size_t ct_shadow_table_size = CT_SHADOW_TABLE_SIZE;
static size_t ct_shadow_table_mask = CT_SHADOW_TABLE_SIZE - 1u;
static int ct_shadow_lock = 0;
static int ct_shadow_table_full_logged = 0;

CT_NOINSTR static void ct_shadow_lock_acquire(void)
{
    while (__atomic_exchange_n(&ct_shadow_lock, 1, __ATOMIC_ACQUIRE) != 0) {
    }
}

CT_NOINSTR static void ct_shadow_lock_release(void)
{
    __atomic_store_n(&ct_shadow_lock, 0, __ATOMIC_RELEASE);
}

CT_NOINSTR static size_t ct_shadow_hash(uintptr_t page, size_t mask)
{
    uintptr_t value = page;
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    return static_cast<size_t>(value) & mask;
}

CT_NOINSTR static int ct_shadow_rehash_entry(struct ct_shadow_page_entry *table,
                                             size_t mask,
                                             size_t size,
                                             const struct ct_shadow_page_entry *entry)
{
    size_t idx = ct_shadow_hash(entry->page, mask);
    for (size_t i = 0; i < size; ++i) {
        size_t pos = (idx + i) & mask;
        struct ct_shadow_page_entry *slot = &table[pos];
        if (slot->state == CT_SHADOW_ENTRY_EMPTY) {
            *slot = *entry;
            return 1;
        }
    }
    return 0;
}

CT_NOINSTR static int ct_shadow_grow_locked(void)
{
    if (ct_shadow_table_bits >= CT_SHADOW_TABLE_MAX_BITS) {
        return 0;
    }

    size_t new_bits = ct_shadow_table_bits + 1u;
    size_t new_size = 1u << new_bits;
    auto *new_table =
        static_cast<struct ct_shadow_page_entry *>(
            std::malloc(sizeof(struct ct_shadow_page_entry) * new_size));
    if (!new_table) {
        return 0;
    }
    std::memset(new_table, 0, sizeof(struct ct_shadow_page_entry) * new_size);

    size_t new_mask = new_size - 1u;
    for (size_t i = 0; i < ct_shadow_table_size; ++i) {
        struct ct_shadow_page_entry *entry = &ct_shadow_table[i];
        if (entry->state != CT_SHADOW_ENTRY_USED) {
            continue;
        }
        (void)ct_shadow_rehash_entry(new_table, new_mask, new_size, entry);
    }

    if (ct_shadow_table != ct_shadow_table_storage) {
        std::free(ct_shadow_table);
    }

    ct_shadow_table = new_table;
    ct_shadow_table_bits = new_bits;
    ct_shadow_table_size = new_size;
    ct_shadow_table_mask = new_mask;
    ct_shadow_table_full_logged = 0;
    return 1;
}

CT_NOINSTR static unsigned char *ct_shadow_get_page_locked(uintptr_t page, int create)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        size_t idx = ct_shadow_hash(page, ct_shadow_table_mask);
        size_t tombstone = static_cast<size_t>(-1);

        for (size_t i = 0; i < ct_shadow_table_size; ++i) {
            size_t pos = (idx + i) & ct_shadow_table_mask;
            struct ct_shadow_page_entry *entry = &ct_shadow_table[pos];

            if (entry->state == CT_SHADOW_ENTRY_USED) {
                if (entry->page == page) {
                    return entry->data;
                }
                continue;
            }

            if (entry->state == CT_SHADOW_ENTRY_TOMB &&
                tombstone == static_cast<size_t>(-1)) {
                tombstone = pos;
                continue;
            }

            if (entry->state == CT_SHADOW_ENTRY_EMPTY) {
                if (!create) {
                    return nullptr;
                }

                if (tombstone != static_cast<size_t>(-1)) {
                    entry = &ct_shadow_table[tombstone];
                }

                unsigned char *data =
                    static_cast<unsigned char *>(std::malloc(CT_SHADOW_PAGE_SIZE));
                if (!data) {
                    return nullptr;
                }
                std::memset(data, 0xFF, CT_SHADOW_PAGE_SIZE);

                entry->page = page;
                entry->data = data;
                entry->state = CT_SHADOW_ENTRY_USED;
                return entry->data;
            }
        }

        if (!create || !ct_shadow_grow_locked()) {
            if (create && !ct_shadow_table_full_logged) {
                ct_shadow_table_full_logged = 1;
                ct_log(CTLevel::Warn,
                       "{}shadow table full ({} entries){}\n",
                       ct_color(CTColor::Red),
                       ct_shadow_table_size,
                       ct_color(CTColor::Reset));
            }
            return nullptr;
        }
    }

    return nullptr;
}

CT_NOINSTR static unsigned char ct_shadow_get_byte_locked(uintptr_t shadow_index)
{
    uintptr_t page = shadow_index >> CT_SHADOW_PAGE_BITS;
    size_t offset = static_cast<size_t>(shadow_index & CT_SHADOW_PAGE_MASK);
    unsigned char *data = ct_shadow_get_page_locked(page, 0);
    if (!data) {
        return 0xFF;
    }
    return data[offset];
}

CT_NOINSTR static void ct_shadow_set_byte_locked(uintptr_t shadow_index, unsigned char value)
{
    uintptr_t page = shadow_index >> CT_SHADOW_PAGE_BITS;
    size_t offset = static_cast<size_t>(shadow_index & CT_SHADOW_PAGE_MASK);
    unsigned char *data = ct_shadow_get_page_locked(page, 1);
    if (!data) {
        return;
    }
    data[offset] = value;
}

CT_NOINSTR void ct_shadow_poison_range(const void *addr, size_t size)
{
    if (!ct_shadow_enabled || !addr || size == 0) {
        return;
    }

    uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    uintptr_t end = start + size;
    if (end <= start) {
        return;
    }

    uintptr_t shadow_start = start >> CT_SHADOW_SHIFT;
    uintptr_t shadow_end = (end - 1) >> CT_SHADOW_SHIFT;

    ct_shadow_lock_acquire();
    for (uintptr_t idx = shadow_start; idx <= shadow_end; ++idx) {
        ct_shadow_set_byte_locked(idx, 0xFF);
    }
    ct_shadow_lock_release();
}

CT_NOINSTR void ct_shadow_unpoison_range(const void *addr, size_t size)
{
    if (!ct_shadow_enabled || !addr || size == 0) {
        return;
    }

    uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    uintptr_t shadow_index = start >> CT_SHADOW_SHIFT;
    size_t full = size / 8;
    size_t tail = size % 8;

    ct_shadow_lock_acquire();
    for (size_t i = 0; i < full; ++i) {
        ct_shadow_set_byte_locked(shadow_index + i, 0);
    }
    if (tail != 0) {
        ct_shadow_set_byte_locked(shadow_index + full, static_cast<unsigned char>(tail));
    }
    ct_shadow_lock_release();
}

CT_NOINSTR int ct_shadow_check_access(const void *ptr,
                                      size_t access_size,
                                      const void *base,
                                      size_t req_size,
                                      size_t alloc_size,
                                      const char *alloc_site,
                                      const char *site,
                                      int is_write,
                                      unsigned char state)
{
    uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t end = start + access_size;
    if (end <= start) {
        return 0;
    }

    uintptr_t shadow_start = start >> CT_SHADOW_SHIFT;
    uintptr_t shadow_end = (end - 1) >> CT_SHADOW_SHIFT;

    int oob = 0;
    ct_shadow_lock_acquire();
    for (uintptr_t idx = shadow_start; idx <= shadow_end; ++idx) {
        unsigned char value = ct_shadow_get_byte_locked(idx);
        if (value == 0) {
            continue;
        }

        uintptr_t block_start = idx << CT_SHADOW_SHIFT;
        uintptr_t block_end = block_start + 8;
        uintptr_t access_start = start > block_start ? start : block_start;
        uintptr_t access_end = end < block_end ? end : block_end;

        if (value == 0xFF) {
            oob = 1;
            break;
        }

        uintptr_t allowed_end = block_start + static_cast<uintptr_t>(value);
        if (access_end > allowed_end) {
            oob = 1;
            break;
        }
    }
    ct_shadow_lock_release();

    if (!oob) {
        return 0;
    }

    ct_report_bounds_error(base,
                           ptr,
                           access_size,
                           site,
                           is_write,
                           req_size,
                           alloc_size,
                           alloc_site,
                           state);
    return 1;
}
