// SPDX-License-Identifier: Apache-2.0
//
// Only an empty slot ends a probe sequence of the allocation table. Once its empty slots are
// all taken, by records or by the tombstones that removed records leave, every insertion and
// every lookup of a block the table does not hold scans the whole table (#214). The table must
// keep empty slots while it tracks as many live blocks as before.
//
// An executable of its own: the tests fill the table up. Each one drops the records it made.
#include "ct_runtime_alloc_internal.h"
#include "ct_runtime_internal.h"
#include "ct_runtime_quarantine.h"

#include <gtest/gtest.h>

#include <cstdint>

// The configuration globals of an instrumented program, as runtime_table_test.cpp defines them,
// but with the allocation trace off.
extern "C"
{
    int __ct_config_shadow = 0;
    int __ct_config_shadow_aggressive = 0;
    int __ct_config_bounds_no_abort = 0;
    int __ct_config_disable_alloc = 0;
    int __ct_config_disable_autofree = 1;
    int __ct_config_disable_alloc_trace = 1;
    int __ct_config_vtable_diag = 0;
}

namespace
{
    void* fillBlock(uintptr_t i)
    {
        return reinterpret_cast<void*>(uintptr_t{0x7f0000000000} + i * 16);
    }

    // ct_alloc_lock held.
    size_t slotsIn(unsigned char state)
    {
        size_t count = 0;
        for (size_t i = 0; i < ct_alloc_table_size; ++i)
        {
            count += ct_alloc_table[i].state == state;
        }
        return count;
    }

    // Drops the record of `block`, live or freed, as the quarantine does once it releases it.
    // ct_alloc_lock held.
    void dropRecord(void* block)
    {
        (void)ct_table_remove(block, nullptr, nullptr, nullptr);
        ct_table_forget_freed(block);
    }

    // The table tracks as many live blocks as before it kept empty slots, 2^20, and still keeps
    // an eighth of its slots empty then.
    TEST(AllocTableTombstones, ATableTracksAsManyLiveBlocksAsBefore)
    {
        ct_lock_acquire();
        const size_t live_before = ct_alloc_count;
        uintptr_t filled = 0;
        while (ct_table_insert(fillBlock(filled), 16, 16, "tombstones:fill", CT_ALLOC_KIND_MALLOC))
        {
            ++filled;
        }
        const size_t tracked = ct_alloc_count;
        const size_t size = ct_alloc_table_size;
        const size_t empty = slotsIn(CT_ENTRY_EMPTY);
        // A live record is still updated at the limit.
        const int updated =
            ct_table_insert(fillBlock(0), 32, 32, "tombstones:update", CT_ALLOC_KIND_MALLOC);
        for (uintptr_t i = 0; i < filled; ++i)
        {
            dropRecord(fillBlock(i));
        }
        ct_lock_release();

        EXPECT_EQ(tracked, size_t{1} << 20);
        EXPECT_EQ(filled, (size_t{1} << 20) - live_before);
        EXPECT_GE(empty, size / 8) << "live blocks took the empty slots of a table of " << size;
        EXPECT_EQ(updated, 1);
    }
} // namespace
