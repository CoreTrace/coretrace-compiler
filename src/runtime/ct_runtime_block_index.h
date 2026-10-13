// SPDX-License-Identifier: Apache-2.0
//
// An index of blocks by address, for the lookup of the block that contains an address: an
// interior pointer, or an object's this pointer (#185). The allocation table keeps one in step
// with its records of live and freed blocks, under its lock; scanning the table instead cost a
// pass over every record, at every virtual call with the vtable module.
//
// A block of size up to 2^L belongs to class L, under the granule of its base, base >> L. A
// block of class L that contains an address starts in the address's granule or the one before
// it, so a lookup visits two granules per class in use. The granules are chained in a hash of
// buckets; nothing is allocated per operation, only when the index grows.
//
// Internal to the runtime; not thread-safe: the caller serialises every call.
#ifndef CT_RUNTIME_BLOCK_INDEX_H
#define CT_RUNTIME_BLOCK_INDEX_H

#include "ct_runtime_internal.h"

#include <cstddef>
#include <cstdint>

struct ct_block_index
{
    uintptr_t* base;
    size_t* size;
    // The next block of the same bucket, or the next free slot.
    uint32_t* next;
    // The first block of each bucket.
    uint32_t* heads;
    size_t capacity;
    // The capacity the index stops growing at, a power of two: at most 2^31, its slots being
    // 32-bit.
    size_t max_capacity;
    // Slots used once; the slots below it are blocks or free.
    size_t used;
    uint32_t free_head;
    size_t bucket_mask;
    // The classes that hold a block, and how many each holds.
    uint64_t classes;
    uint32_t class_count[64];
    // Set when a block could not be added for want of memory: the index then misses blocks,
    // and its users must not rely on it.
    bool incomplete;
    // Storage the index started with, which it does not free.
    void* initial_storage;
};

// Bytes of storage an index of `capacity` blocks needs, `capacity` a power of two.
constexpr size_t ct_block_index_storage_size(size_t capacity)
{
    return capacity * (sizeof(uintptr_t) + sizeof(size_t) + sizeof(uint32_t)) +
           2 * capacity * sizeof(uint32_t);
}

// An empty index over `storage`, of ct_block_index_storage_size(capacity) bytes, aligned for
// uintptr_t; it grows with malloc beyond `capacity`, a power of two.
CT_NOINSTR void ct_block_index_init(ct_block_index* index, void* storage, size_t capacity);
// Frees what the index allocated; it is then empty and must be initialised again.
CT_NOINSTR void ct_block_index_destroy(ct_block_index* index);

// Adds a block of `size` bytes, not 0, at `base`, which the index does not hold. False, and
// the index marked incomplete, when it could not grow.
CT_NODISCARD CT_NOINSTR bool ct_block_index_add(ct_block_index* index, uintptr_t base, size_t size);
// Removes the block of `size` bytes at `base`; nothing when the index does not hold it.
CT_NOINSTR void ct_block_index_remove(ct_block_index* index, uintptr_t base, size_t size);
// A block that contains `address`: its base and size. Among blocks that overlap, one of the
// smallest class.
CT_NODISCARD CT_NOINSTR bool ct_block_index_find(const ct_block_index* index, uintptr_t address,
                                                 uintptr_t* base_out, size_t* size_out);

#endif // CT_RUNTIME_BLOCK_INDEX_H
