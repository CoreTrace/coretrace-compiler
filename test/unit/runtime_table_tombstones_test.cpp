// SPDX-License-Identifier: Apache-2.0
//
// Only an empty slot ends a probe sequence of the allocation table. Once its empty slots are
// all taken, by records or by the tombstones that removed records leave, every insertion and
// every lookup of a block the table does not hold scans the whole table (#214). The table must
// keep empty slots while it tracks as many live blocks as before, and the quarantine must give
// way to new blocks without leaving any of its blocks unrecorded.
//
// An executable of its own: the tests fill the table up. Each one drops the records it made
// and empties the quarantine it filled.
#include "ct_runtime_alloc_internal.h"
#include "ct_runtime_internal.h"
#include "ct_runtime_quarantine.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

// The configuration globals of an instrumented program, as runtime_table_test.cpp defines them,
// but with the allocation trace off: the tests allocate millions of blocks.
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
    constexpr size_t kBlockSize = 16;

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

    // What the quarantine holds when each of its blocks has its freed record: the size of every
    // freed record and the place of its block in the queue. ct_alloc_lock held.
    size_t bytesOfFreedRecords()
    {
        size_t bytes = 0;
        for (size_t i = 0; i < ct_alloc_table_size; ++i)
        {
            const ct_alloc_entry& entry = ct_alloc_table[i];
            if (entry.state == CT_ENTRY_FREED || entry.state == CT_ENTRY_AUTOFREED)
            {
                bytes += entry.size + sizeof(ct_quarantine_item);
            }
        }
        return bytes;
    }

    // The state of the record of `ptr`, or 0 when there is none.
    unsigned char recordedState(void* ptr)
    {
        ct_lock_acquire();
        unsigned char state = 0;
        const int found = ct_table_lookup(ptr, nullptr, nullptr, nullptr, &state);
        ct_lock_release();
        return found ? state : 0;
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

    // Blocks the program frees, which the quarantine keeps, and new blocks allocated after them,
    // more of both than the largest table keeps room for.
    class QuarantineAtItsLimit : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            for (size_t i = 0; i < kFreed; ++i)
            {
                freed_.push_back(__ct_malloc(kBlockSize, "tombstones:freed"));
            }
            for (void* block : freed_)
            {
                __ct_free(block, "tombstones:free");
            }
            for (void* block : freed_)
            {
                recorded_before_ += recordedState(block) == CT_ENTRY_FREED;
            }
            for (size_t i = 0; i < kLive; ++i)
            {
                live_.push_back(__ct_malloc(kBlockSize, "tombstones:live"));
            }
        }

        void TearDown() override
        {
            for (void* block : live_)
            {
                __ct_free(block, "tombstones:free");
            }
            ct_quarantine_set_limit(0);
            ct_quarantine_set_limit(kDefaultLimit);
        }

        // How many of the blocks freed have no freed record left: those taken out of the
        // quarantine.
        size_t takenOut() const
        {
            size_t taken_out = 0;
            for (void* block : freed_)
            {
                taken_out += recordedState(block) != CT_ENTRY_FREED;
            }
            return taken_out;
        }

        static constexpr size_t kDefaultLimit = size_t{256} << 20;
        static constexpr size_t kFreed = size_t{1} << 20;
        static constexpr size_t kLive = 960'000;
        std::vector<void*> freed_;
        std::vector<void*> live_;
        size_t recorded_before_ = 0;
    };

    // The quarantine gives its oldest blocks, released for real, to make room for new ones, and
    // every block it still holds keeps its freed record: a second release of it is reported.
    TEST_F(QuarantineAtItsLimit, TheQuarantineGivesItsOldestBlocksWayToNewOnes)
    {
        size_t untracked = 0;
        for (void* block : live_)
        {
            untracked += recordedState(block) != CT_ENTRY_USED;
        }
        const size_t taken_out = takenOut();
        // Taken out oldest first: the first blocks freed have no freed record left, their
        // addresses perhaps handed out again to live blocks; the others are still freed.
        size_t out_of_order = 0;
        for (size_t i = 0; i < freed_.size(); ++i)
        {
            const bool recorded_as_freed = recordedState(freed_[i]) == CT_ENTRY_FREED;
            out_of_order += recorded_as_freed != (i >= taken_out);
        }
        const size_t quarantined_bytes = ct_quarantine_bytes();
        ct_lock_acquire();
        const size_t freed_record_bytes = bytesOfFreedRecords();
        const size_t size = ct_alloc_table_size;
        const size_t empty = slotsIn(CT_ENTRY_EMPTY);
        const int second_release = ct_table_remove(freed_.back(), nullptr, nullptr, nullptr);
        ct_lock_release();

        EXPECT_EQ(untracked, 0u);
        EXPECT_EQ(recorded_before_, kFreed);
        EXPECT_GT(taken_out, 0u) << "the table made no room";
        EXPECT_EQ(out_of_order, 0u);
        EXPECT_EQ(freed_record_bytes, quarantined_bytes) << "a quarantined block lost its record";
        EXPECT_GE(empty, size / 16);
        EXPECT_EQ(second_release, -1);
    }

    // A block taken out of the quarantine goes back to the allocator, which may hand its address
    // out again: the new block there is tracked like any other.
    TEST_F(QuarantineAtItsLimit, ABlockTakenOutOfTheQuarantineIsTrackedWhenItsAddressComesBack)
    {
        const size_t taken_out = takenOut();
        ASSERT_GT(taken_out, 0u);
        const std::set<void*> released(freed_.begin(), freed_.begin() + taken_out);

        std::vector<void*> again;
        size_t reused = 0;
        size_t reused_and_tracked = 0;
        for (int i = 0; i < 100'000 && reused < 100; ++i)
        {
            void* block = __ct_malloc(kBlockSize, "tombstones:again");
            again.push_back(block);
            if (released.count(block))
            {
                ++reused;
                reused_and_tracked += recordedState(block) == CT_ENTRY_USED;
            }
        }
        for (void* block : again)
        {
            __ct_free(block, "tombstones:free");
        }

        EXPECT_GT(reused, 0u) << "the allocator handed out none of the released addresses";
        EXPECT_EQ(reused_and_tracked, reused);
    }

    // Threads allocate and free while the table makes room: each one releases the blocks it took
    // out of the quarantine, every new block is tracked, and the quarantine keeps its records.
    TEST_F(QuarantineAtItsLimit, MakingRoomKeepsTrackOfConcurrentAllocations)
    {
        constexpr int kThreads = 4;
        constexpr int kRounds = 50'000;
        std::atomic<size_t> untracked{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t)
        {
            threads.emplace_back(
                [&untracked]
                {
                    for (int i = 0; i < kRounds; ++i)
                    {
                        void* block = __ct_malloc(kBlockSize, "tombstones:thread");
                        if (recordedState(block) != CT_ENTRY_USED)
                        {
                            untracked.fetch_add(1);
                        }
                        __ct_free(block, "tombstones:free");
                    }
                });
        }
        for (std::thread& thread : threads)
        {
            thread.join();
        }
        const size_t quarantined_bytes = ct_quarantine_bytes();
        ct_lock_acquire();
        const size_t freed_record_bytes = bytesOfFreedRecords();
        ct_lock_release();

        EXPECT_EQ(untracked.load(), 0u);
        EXPECT_EQ(freed_record_bytes, quarantined_bytes) << "a quarantined block lost its record";
    }
} // namespace
