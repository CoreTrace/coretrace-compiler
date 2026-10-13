// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_block_index.h"

#include <bit>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t kNone = 0xffffffffu;
    // Blocks of up to 16 bytes share the first class.
    constexpr unsigned kFirstClass = 4;
    constexpr size_t kMaxCapacity = size_t{1} << 31;

    CT_NODISCARD CT_NOINSTR unsigned ct_block_class(size_t size)
    {
        unsigned level = kFirstClass;
        while (level < 63 && (static_cast<size_t>(1) << level) < size)
        {
            ++level;
        }
        return level;
    }

    CT_NODISCARD CT_NOINSTR size_t ct_block_bucket(const ct_block_index* index, unsigned level,
                                                   uintptr_t granule)
    {
        uint64_t hash = (static_cast<uint64_t>(granule) * 0x9e3779b97f4a7c15ull) ^ level;
        hash ^= hash >> 29;
        return static_cast<size_t>(hash) & index->bucket_mask;
    }

    // Lays the arrays of `capacity` blocks and 2 * capacity buckets out in `storage`.
    CT_NOINSTR void ct_block_index_layout(ct_block_index* index, void* storage, size_t capacity)
    {
        auto* bytes = static_cast<unsigned char*>(storage);
        index->base = reinterpret_cast<uintptr_t*>(bytes);
        bytes += capacity * sizeof(uintptr_t);
        index->size = reinterpret_cast<size_t*>(bytes);
        bytes += capacity * sizeof(size_t);
        index->next = reinterpret_cast<uint32_t*>(bytes);
        bytes += capacity * sizeof(uint32_t);
        index->heads = reinterpret_cast<uint32_t*>(bytes);
        index->capacity = capacity;
        index->bucket_mask = 2 * capacity - 1;
        for (size_t i = 0; i < 2 * capacity; ++i)
        {
            index->heads[i] = kNone;
        }
    }

    CT_NOINSTR void ct_block_index_link(ct_block_index* index, uint32_t slot)
    {
        const unsigned level = ct_block_class(index->size[slot]);
        const size_t bucket = ct_block_bucket(index, level, index->base[slot] >> level);
        index->next[slot] = index->heads[bucket];
        index->heads[bucket] = slot;
        index->classes |= static_cast<uint64_t>(1) << level;
        ++index->class_count[level];
    }

    // Moves the blocks into storage for twice the capacity. The index stays as it was when
    // there is no memory for it.
    CT_NODISCARD CT_NOINSTR bool ct_block_index_grow(ct_block_index* index)
    {
        if (index->capacity > index->max_capacity / 2)
        {
            return false;
        }
        const size_t capacity = index->capacity * 2;
        void* storage = std::malloc(ct_block_index_storage_size(capacity));
        if (!storage)
        {
            return false;
        }

        ct_block_index old = *index;
        ct_block_index_layout(index, storage, capacity);
        index->classes = 0;
        std::memset(index->class_count, 0, sizeof(index->class_count));
        index->used = 0;
        index->free_head = kNone;
        // Only the blocks the index holds: a free slot is on the free list, not in a bucket.
        for (size_t bucket = 0; bucket <= old.bucket_mask; ++bucket)
        {
            for (uint32_t slot = old.heads[bucket]; slot != kNone; slot = old.next[slot])
            {
                const uint32_t moved = static_cast<uint32_t>(index->used++);
                index->base[moved] = old.base[slot];
                index->size[moved] = old.size[slot];
                ct_block_index_link(index, moved);
            }
        }
        if (old.base != old.initial_storage)
        {
            std::free(old.base);
        }
        return true;
    }
} // namespace

CT_NOINSTR void ct_block_index_init(ct_block_index* index, void* storage, size_t capacity)
{
    std::memset(index, 0, sizeof(*index));
    ct_block_index_layout(index, storage, capacity);
    index->free_head = kNone;
    index->max_capacity = kMaxCapacity;
    index->initial_storage = storage;
}

CT_NOINSTR void ct_block_index_destroy(ct_block_index* index)
{
    if (index->base && index->base != index->initial_storage)
    {
        std::free(index->base);
    }
    std::memset(index, 0, sizeof(*index));
}

CT_NODISCARD CT_NOINSTR bool ct_block_index_add(ct_block_index* index, uintptr_t base, size_t size)
{
    uint32_t slot = index->free_head;
    if (slot != kNone)
    {
        index->free_head = index->next[slot];
    }
    else
    {
        if (index->used == index->capacity && !ct_block_index_grow(index))
        {
            index->incomplete = true;
            return false;
        }
        slot = static_cast<uint32_t>(index->used++);
    }
    index->base[slot] = base;
    index->size[slot] = size;
    ct_block_index_link(index, slot);
    return true;
}

CT_NOINSTR void ct_block_index_remove(ct_block_index* index, uintptr_t base, size_t size)
{
    const unsigned level = ct_block_class(size);
    uint32_t* link = &index->heads[ct_block_bucket(index, level, base >> level)];
    while (*link != kNone)
    {
        const uint32_t slot = *link;
        if (index->base[slot] == base && index->size[slot] == size)
        {
            *link = index->next[slot];
            index->next[slot] = index->free_head;
            index->free_head = slot;
            if (--index->class_count[level] == 0)
            {
                index->classes &= ~(static_cast<uint64_t>(1) << level);
            }
            return;
        }
        link = &index->next[slot];
    }
}

CT_NODISCARD CT_NOINSTR bool ct_block_index_find(const ct_block_index* index, uintptr_t address,
                                                 uintptr_t* base_out, size_t* size_out)
{
    for (uint64_t classes = index->classes; classes != 0; classes &= classes - 1)
    {
        const unsigned level = static_cast<unsigned>(std::countr_zero(classes));
        const uintptr_t granule = address >> level;
        // A block of this class that contains the address starts in its granule or the one
        // before.
        for (uintptr_t candidate : {granule, granule - 1})
        {
            for (uint32_t slot = index->heads[ct_block_bucket(index, level, candidate)];
                 slot != kNone; slot = index->next[slot])
            {
                if (address - index->base[slot] < index->size[slot])
                {
                    *base_out = index->base[slot];
                    *size_out = index->size[slot];
                    return true;
                }
            }
        }
    }
    return false;
}
