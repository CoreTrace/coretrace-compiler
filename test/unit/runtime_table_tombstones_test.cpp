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

    // Records made and removed one after the other leave tombstones, at addresses never reused:
    // the table must drop them before they take its empty slots, without growing because of
    // them, and keep its other records.
    TEST(AllocTableTombstones, TombstonesAreDroppedBeforeTheyTakeTheEmptySlots)
    {
        void* live = reinterpret_cast<void*>(uintptr_t{0x7e0000001000});
        void* freed = reinterpret_cast<void*>(uintptr_t{0x7e0000002000});
        ct_lock_acquire();
        ASSERT_TRUE(ct_table_insert(live, 24, 32, "tombstones:live", CT_ALLOC_KIND_NEW));
        ASSERT_TRUE(ct_table_insert(freed, 8, 16, "tombstones:freed", CT_ALLOC_KIND_MALLOC));
        ASSERT_EQ(ct_table_remove(freed, nullptr, nullptr, nullptr), 1);
        const size_t live_count = ct_alloc_count;
        const size_t size_before = ct_alloc_table_size;

        // More records than the largest table has slots, each dropped as the next one comes.
        constexpr uintptr_t kRecords = 4'000'000;
        uintptr_t refused = 0;
        for (uintptr_t i = 0; i < kRecords; ++i)
        {
            if (!ct_table_insert(fillBlock(i), 16, 16, "tombstones:fill", CT_ALLOC_KIND_MALLOC))
            {
                ++refused;
                continue;
            }
            dropRecord(fillBlock(i));
        }

        const size_t size = ct_alloc_table_size;
        const size_t empty = slotsIn(CT_ENTRY_EMPTY);
        const size_t count_after = ct_alloc_count;
        size_t live_size = 0;
        size_t live_req_size = 0;
        const char* live_site = nullptr;
        unsigned char live_state = CT_ENTRY_EMPTY;
        const int live_found =
            ct_table_lookup(live, &live_size, &live_req_size, &live_site, &live_state);
        unsigned char freed_state = CT_ENTRY_EMPTY;
        const int freed_found = ct_table_lookup(freed, nullptr, nullptr, nullptr, &freed_state);
        void* base = nullptr;
        const int contained = ct_table_lookup_containing(static_cast<char*>(live) + 8, &base,
                                                         nullptr, nullptr, nullptr, nullptr);
        dropRecord(freed);
        dropRecord(live);
        ct_lock_release();

        EXPECT_EQ(refused, 0u);
        EXPECT_EQ(size, size_before) << "tombstones made the table grow";
        EXPECT_GE(empty, size / 16) << "tombstones took the empty slots of a table of " << size;
        EXPECT_EQ(count_after, live_count);
        ASSERT_EQ(live_found, 1);
        EXPECT_EQ(live_state, CT_ENTRY_USED);
        EXPECT_EQ(live_size, 32u);
        EXPECT_EQ(live_req_size, 24u);
        EXPECT_STREQ(live_site, "tombstones:live");
        ASSERT_EQ(freed_found, 1);
        EXPECT_EQ(freed_state, CT_ENTRY_FREED);
        ASSERT_EQ(contained, 1);
        EXPECT_EQ(base, live);
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
