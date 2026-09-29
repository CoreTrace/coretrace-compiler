// SPDX-License-Identifier: Apache-2.0
//
// Blocks the program releases stay out of the allocator while the table records them as
// freed, so that no other allocation, even one the runtime does not see, receives their
// address. Every test releases what it allocates and empties the quarantine it filled.
#include "ct_runtime_internal.h"
#include "ct_runtime_quarantine.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
    constexpr size_t kBlockSize = 32;

    // Record of `ptr` in the allocation table: its state, or 0 when there is none.
    unsigned char recordedState(void* ptr)
    {
        ct_lock_acquire();
        unsigned char state = 0;
        const int found = ct_table_lookup(ptr, nullptr, nullptr, nullptr, &state);
        ct_lock_release();
        return found ? state : 0;
    }

    // Empties the quarantine a test filled and restores the default limit.
    class QuarantineTest : public ::testing::Test
    {
      protected:
        void TearDown() override
        {
            ct_quarantine_set_limit(0);
            ct_quarantine_set_limit(kDefaultLimit);
        }

        static constexpr size_t kDefaultLimit = size_t{256} << 20;
    };

    // An allocation the runtime does not see, made right after a tracked release, would
    // commonly get the released block's address back from the allocator.
    TEST_F(QuarantineTest, ReleasedBlockIsNotHandedOutAgain)
    {
        int reused = 0;
        for (int i = 0; i < 64; ++i)
        {
            void* tracked = __ct_malloc(kBlockSize, "test:tracked");
            ASSERT_NE(tracked, nullptr);
            __ct_free(tracked, "test:free");

            void* untracked = std::malloc(kBlockSize);
            reused += untracked == tracked;
            std::free(untracked);
        }
        EXPECT_EQ(reused, 0) << "of 64 releases";
    }

    TEST_F(QuarantineTest, ReleasedBlockStaysRecordedAsFreed)
    {
        void* tracked = __ct_malloc(kBlockSize, "test:tracked");
        ASSERT_NE(tracked, nullptr);
        __ct_free(tracked, "test:free");
        EXPECT_EQ(recordedState(tracked), CT_ENTRY_FREED);
    }

    // Past the limit, the oldest blocks are released for real: their freed records go with
    // them, since the allocator may now hand their addresses to anyone.
    TEST_F(QuarantineTest, EvictedBlockLeavesNoFreedRecord)
    {
        ct_quarantine_set_limit(2 * kBlockSize);
        // Allocated before any release, so that no allocation reuses an evicted address.
        std::vector<void*> released;
        for (int i = 0; i < 4; ++i)
        {
            void* tracked = __ct_malloc(kBlockSize, "test:tracked");
            ASSERT_NE(tracked, nullptr);
            released.push_back(tracked);
        }
        for (void* tracked : released)
        {
            __ct_free(tracked, "test:free");
        }
        EXPECT_EQ(recordedState(released.front()), 0);
        EXPECT_EQ(recordedState(released.back()), CT_ENTRY_FREED);
        EXPECT_LE(ct_quarantine_bytes(), 2 * kBlockSize);
    }

    // realloc copies into a new block and quarantines the old one, which stays a
    // use-after-free target instead of being released, or grown in place, by the C library.
    TEST_F(QuarantineTest, ReallocQuarantinesTheOldBlock)
    {
        int in_place = 0;
        int reused = 0;
        for (int i = 0; i < 64; ++i)
        {
            auto* old_block = static_cast<unsigned char*>(__ct_malloc(kBlockSize, "test:tracked"));
            ASSERT_NE(old_block, nullptr);
            std::memset(old_block, 0x5a, kBlockSize);

            auto* new_block = static_cast<unsigned char*>(
                __ct_realloc(old_block, 4 * kBlockSize, "test:realloc"));
            ASSERT_NE(new_block, nullptr);
            in_place += new_block == old_block;
            for (size_t byte = 0; byte < kBlockSize; ++byte)
            {
                ASSERT_EQ(new_block[byte], 0x5a) << "byte " << byte;
            }
            if (new_block != old_block)
            {
                EXPECT_EQ(recordedState(old_block), CT_ENTRY_FREED);
            }
            EXPECT_EQ(recordedState(new_block), CT_ENTRY_USED);

            void* untracked = std::malloc(kBlockSize);
            reused += untracked == old_block;
            std::free(untracked);
            __ct_free(new_block, "test:free");
        }
        EXPECT_EQ(in_place, 0) << "of 64 reallocations";
        EXPECT_EQ(reused, 0) << "of 64 reallocations";
    }
    // realloc of a released block, still in the quarantine: the C library must not see it,
    // since the quarantine will release it once more.
    TEST_F(QuarantineTest, ReallocOfAReleasedBlockLeavesItAlone)
    {
        void* released = __ct_malloc(kBlockSize, "test:tracked");
        ASSERT_NE(released, nullptr);
        __ct_free(released, "test:free");

        EXPECT_EQ(__ct_realloc(released, 4 * kBlockSize, "test:realloc"), nullptr);
        EXPECT_EQ(recordedState(released), CT_ENTRY_FREED);
    }
} // namespace
