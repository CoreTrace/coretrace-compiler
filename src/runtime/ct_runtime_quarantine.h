// SPDX-License-Identifier: Apache-2.0
//
// Blocks the program released, kept away from the allocator while the allocation table
// records them as freed. While a block is here, no other allocation, not even one the
// runtime does not see (strdup, a library), can receive its address: an access through a
// dangling pointer is reported as a use-after-free, and an access to a live block never
// is. Past a byte limit, CT_QUARANTINE_MB (256 by default), counting the blocks and the
// queue that holds them, the oldest blocks are released for real once their freed records
// are dropped, and accesses to them are no longer reported.
//
// The queue is shared by the POSIX and Windows allocators; each provides the two hooks
// declared at the end, which know its table and how its blocks are released.
#ifndef CT_RUNTIME_QUARANTINE_H
#define CT_RUNTIME_QUARANTINE_H

#include "ct_runtime_internal.h"

#include <cstddef>

// A released block and what its allocator needs to release it for real: the release
// function the program called and the kind of the allocation.
struct ct_quarantine_item
{
    void* ptr;
    size_t size;
    unsigned char api;
    unsigned char kind;
};

// Takes a block whose table entry was just marked as freed, instead of releasing it.
// ct_alloc_lock not held.
CT_NOINSTR void ct_quarantine_push(const ct_quarantine_item& item);

// The byte limit, read from CT_QUARANTINE_MB until set. Lowering it releases the oldest
// blocks down to the new limit; 0 releases every block at once. ct_alloc_lock not held.
CT_NOINSTR void ct_quarantine_set_limit(size_t bytes);
CT_NODISCARD CT_NOINSTR size_t ct_quarantine_bytes(void);

// Allocator hooks.
// Drops the freed record of `ptr`, whose block the allocator may now hand out again.
// ct_alloc_lock held.
CT_NOINSTR void ct_table_forget_freed(void* ptr);
// Releases a block leaving the quarantine, its shadow restored to the state of memory the
// runtime does not track. ct_alloc_lock not held.
CT_NOINSTR void ct_quarantine_release(const ct_quarantine_item& item);

#endif // CT_RUNTIME_QUARANTINE_H
