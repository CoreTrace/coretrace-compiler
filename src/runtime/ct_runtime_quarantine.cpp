// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_quarantine.h"

#include <cerrno>
#include <cstdlib>

namespace
{
    constexpr size_t kDefaultLimitMb = 256;
    constexpr size_t kBytesPerMb = size_t{1} << 20;
    constexpr size_t kInitialCapacity = 1024;
    // Blocks released per pass once the lock is dropped.
    constexpr size_t kReleaseBatch = 64;

    // A ring buffer, oldest block first. Every field is guarded by ct_alloc_lock.
    ct_quarantine_item* ct_queue = nullptr;
    size_t ct_queue_capacity = 0;
    size_t ct_queue_head = 0;
    size_t ct_queue_count = 0;
    size_t ct_queue_bytes = 0;
    size_t ct_queue_limit = 0;
    bool ct_queue_limit_set = false;

    // What a block costs the quarantine: its memory and its place in the queue, so that the
    // limit also bounds the queue when many small blocks are released.
    CT_NODISCARD CT_NOINSTR size_t ct_cost(const ct_quarantine_item& item)
    {
        return item.size + sizeof(ct_quarantine_item);
    }

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: read once, under the lock.
#endif
    CT_NODISCARD CT_NOINSTR size_t ct_limit_from_env()
    {
        const char* value = std::getenv("CT_QUARANTINE_MB");
        if (!value || !*value)
        {
            return kDefaultLimitMb * kBytesPerMb;
        }
        char* end = nullptr;
        errno = 0;
        const unsigned long long mb = std::strtoull(value, &end, 10);
        if (errno != 0 || end == value || *end != '\0')
        {
            return kDefaultLimitMb * kBytesPerMb;
        }
        if (mb > static_cast<unsigned long long>(static_cast<size_t>(-1) / kBytesPerMb))
        {
            return static_cast<size_t>(-1);
        }
        return static_cast<size_t>(mb) * kBytesPerMb;
    }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    // ct_alloc_lock held.
    CT_NOINSTR void ct_ensure_limit()
    {
        if (!ct_queue_limit_set)
        {
            ct_queue_limit = ct_limit_from_env();
            ct_queue_limit_set = true;
        }
    }

    // Doubles the ring, keeping the blocks in order. ct_alloc_lock held.
    CT_NODISCARD CT_NOINSTR bool ct_grow_queue()
    {
        const size_t capacity = ct_queue_capacity ? ct_queue_capacity * 2 : kInitialCapacity;
        auto* queue =
            static_cast<ct_quarantine_item*>(std::malloc(capacity * sizeof(ct_quarantine_item)));
        if (!queue)
        {
            return false;
        }
        for (size_t i = 0; i < ct_queue_count; ++i)
        {
            queue[i] = ct_queue[(ct_queue_head + i) % ct_queue_capacity];
        }
        std::free(ct_queue);
        ct_queue = queue;
        ct_queue_capacity = capacity;
        ct_queue_head = 0;
        return true;
    }

    // Takes the oldest blocks past the limit out of the queue, their freed records dropped.
    // ct_alloc_lock held.
    CT_NODISCARD CT_NOINSTR size_t ct_take_over_limit(ct_quarantine_item* out, size_t max)
    {
        size_t taken = 0;
        while (taken < max && ct_queue_count > 0 && ct_queue_bytes > ct_queue_limit)
        {
            const ct_quarantine_item item = ct_queue[ct_queue_head];
            ct_queue_head = (ct_queue_head + 1) % ct_queue_capacity;
            --ct_queue_count;
            ct_queue_bytes -= ct_cost(item);
            ct_table_forget_freed(item.ptr);
            out[taken++] = item;
        }
        return taken;
    }

    // Releases blocks until the queue is within its limit. ct_alloc_lock not held.
    CT_NOINSTR void ct_release_over_limit()
    {
        ct_quarantine_item batch[kReleaseBatch];
        size_t taken = 0;
        do
        {
            ct_lock_acquire();
            taken = ct_take_over_limit(batch, kReleaseBatch);
            ct_lock_release();
            for (size_t i = 0; i < taken; ++i)
            {
                ct_quarantine_release(batch[i]);
            }
        } while (taken == kReleaseBatch);
    }
} // namespace

CT_NOINSTR void ct_quarantine_push(const ct_quarantine_item& item)
{
    ct_lock_acquire();
    ct_ensure_limit();
    // A block larger than the whole quarantine, or one there is no room left for, is
    // released at once, as if it had been evicted.
    if (ct_cost(item) > ct_queue_limit || (ct_queue_count == ct_queue_capacity && !ct_grow_queue()))
    {
        ct_table_forget_freed(item.ptr);
        ct_lock_release();
        ct_quarantine_release(item);
        return;
    }
    ct_queue[(ct_queue_head + ct_queue_count) % ct_queue_capacity] = item;
    ++ct_queue_count;
    ct_queue_bytes += ct_cost(item);
    ct_lock_release();

    ct_release_over_limit();
}

CT_NOINSTR void ct_quarantine_set_limit(size_t bytes)
{
    ct_lock_acquire();
    ct_queue_limit = bytes;
    ct_queue_limit_set = true;
    ct_lock_release();

    ct_release_over_limit();
}

CT_NODISCARD CT_NOINSTR size_t ct_quarantine_bytes(void)
{
    ct_lock_acquire();
    const size_t bytes = ct_queue_bytes;
    ct_lock_release();
    return bytes;
}
