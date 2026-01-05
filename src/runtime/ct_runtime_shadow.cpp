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
#define CT_SHADOW_TABLE_SIZE (1u << CT_SHADOW_TABLE_BITS)

struct ct_shadow_page_entry {
    uintptr_t page;
    unsigned char *data;
    unsigned char state;
};

static struct ct_shadow_page_entry ct_shadow_table[CT_SHADOW_TABLE_SIZE];
static int ct_shadow_lock = 0;

CT_NOINSTR static void ct_shadow_lock_acquire(void)
{
    while (__atomic_exchange_n(&ct_shadow_lock, 1, __ATOMIC_ACQUIRE) != 0) {
    }
}

CT_NOINSTR static void ct_shadow_lock_release(void)
{
    __atomic_store_n(&ct_shadow_lock, 0, __ATOMIC_RELEASE);
}

CT_NOINSTR static size_t ct_shadow_hash(uintptr_t page)
{
    uintptr_t value = page;
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    return static_cast<size_t>(value) & (CT_SHADOW_TABLE_SIZE - 1u);
}

CT_NOINSTR static unsigned char *ct_shadow_get_page_locked(uintptr_t page, int create)
{
    size_t idx = ct_shadow_hash(page);
    size_t tombstone = static_cast<size_t>(-1);

    for (size_t i = 0; i < CT_SHADOW_TABLE_SIZE; ++i) {
        size_t pos = (idx + i) & (CT_SHADOW_TABLE_SIZE - 1u);
        struct ct_shadow_page_entry *entry = &ct_shadow_table[pos];

        if (entry->state == CT_SHADOW_ENTRY_USED) {
            if (entry->page == page) {
                return entry->data;
            }
            continue;
        }

        if (entry->state == CT_SHADOW_ENTRY_TOMB && tombstone == static_cast<size_t>(-1)) {
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
