// SPDX-License-Identifier: Apache-2.0
//
// __ct_check_bounds against fake allocations and registered stack buffers: nothing is
// dereferenced. Shadow memory is
// an allow-list (bytes are valid only once unpoisoned), so shadow-mode tests mark bytes
// valid exactly as the allocator does for live allocations. The runtime state touched
// here (features, abort mode, table entries, shadow bytes) is restored by every test.
#include "ct_runtime_internal.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr unsigned char kKindMalloc = 0;
    constexpr size_t kRequested = 8;
    constexpr size_t kUsable = 16;

    char* fakeBlock(uintptr_t index)
    {
        return reinterpret_cast<char*>(uintptr_t{0x60000000u} + index * 64u);
    }

    class BoundsCheck : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            savedFeatures_ = ct_get_features();
            savedAbort_ = ct_bounds_abort_enabled();
            ct_set_enabled(CT_FEATURE_BOUNDS, 1);
            ct_set_bounds_abort(0);
        }

        void TearDown() override
        {
            for (const auto& [start, size] : valid_)
            {
                ct_shadow_poison_range(start, size);
            }
            ct_lock_acquire();
            for (char* block : inserted_)
            {
                (void)ct_table_remove(block, nullptr, nullptr, nullptr);
            }
            ct_lock_release();
            ct_set_bounds_abort(savedAbort_);
            for (uint64_t feature : {CT_FEATURE_BOUNDS, CT_FEATURE_SHADOW, CT_FEATURE_SHADOW_AGGR})
            {
                ct_set_enabled(feature, (savedFeatures_ & feature) != 0);
            }
        }

        char* allocate(uintptr_t index)
        {
            char* block = fakeBlock(index);
            ct_lock_acquire();
            EXPECT_EQ(ct_table_insert(block, kRequested, kUsable, "test:alloc", kKindMalloc), 1);
            ct_lock_release();
            inserted_.push_back(block);
            return block;
        }

        // Marks bytes valid in shadow memory, as the allocator does for live allocations.
        void markValid(const char* start, size_t size)
        {
            ct_shadow_unpoison_range(start, size);
            valid_.push_back({start, size});
        }

        // Runs one check and returns what the runtime wrote to stderr.
        static std::string check(const char* base, const char* ptr, size_t size)
        {
            ::testing::internal::CaptureStderr();
            __ct_check_bounds(base, ptr, size, "test:access", 0);
            return ::testing::internal::GetCapturedStderr();
        }

      private:
        uint64_t savedFeatures_ = 0;
        int savedAbort_ = 1;
        std::vector<char*> inserted_;
        std::vector<std::pair<const char*, size_t>> valid_;
    };

    TEST_F(BoundsCheck, ReportsAnAccessBeforeTheAllocation)
    {
        char* block = allocate(1);
        EXPECT_NE(check(block, block - 4, 4).find("heap-buffer-overflow"), std::string::npos);
    }

    TEST_F(BoundsCheck, ReportsAnAccessPastTheRequestedSize)
    {
        char* block = allocate(2);
        EXPECT_NE(check(block, block + kRequested, 4).find("heap-buffer-overflow"),
                  std::string::npos);
    }

    TEST_F(BoundsCheck, AcceptsAnAccessInsideTheAllocation)
    {
        char* block = allocate(3);
        EXPECT_EQ(check(block, block + 4, 4), "");
    }

    // Shadow bytes only say which bytes are valid, not which object they belong to: when
    // the bytes just before a block are a neighbouring allocation's live data, an access
    // there through a pointer derived from the block looks valid to them. The
    // allocation the base belongs to still bounds the access.
    TEST_F(BoundsCheck, ShadowModeReportsAnAccessIntoTheNeighbourBeforeAKnownAllocation)
    {
        ct_set_enabled(CT_FEATURE_SHADOW, 1);
        char* block = allocate(4);
        markValid(block - 16, 16);
        markValid(block, kRequested);
        EXPECT_NE(check(block, block - 4, 4).find("heap-buffer-overflow"), std::string::npos);
    }

    // Stack objects are registered by their frame for as long as it runs.
    TEST_F(BoundsCheck, ReportsAnAccessPastARegisteredStackObject)
    {
        char buffer[16] = {};
        const size_t depth = __ct_stack_push(buffer, sizeof(buffer), "test:stack");
        const std::string report = check(buffer, buffer + 16, 1);
        __ct_stack_pop(depth);
        EXPECT_NE(report.find("stack-buffer-overflow"), std::string::npos) << report;
    }

    TEST_F(BoundsCheck, AcceptsAnAccessInsideARegisteredStackObject)
    {
        char buffer[16] = {};
        const size_t depth = __ct_stack_push(buffer, sizeof(buffer), "test:stack");
        const std::string report = check(buffer, buffer + 12, 4);
        __ct_stack_pop(depth);
        EXPECT_EQ(report, "");
    }

    TEST_F(BoundsCheck, IgnoresAStackObjectOnceItsFrameReturned)
    {
        char buffer[16] = {};
        __ct_stack_pop(__ct_stack_push(buffer, sizeof(buffer), "test:stack"));
        EXPECT_EQ(check(buffer, buffer + 16, 1), "");
    }

    // An inner frame registered later wins over an object an unwound frame left behind
    // at the same address.
    TEST_F(BoundsCheck, ChecksAgainstTheNewestStackObjectAtAnAddress)
    {
        char buffer[16] = {};
        const size_t depth = __ct_stack_push(buffer, 4, "test:stale");
        (void)__ct_stack_push(buffer, sizeof(buffer), "test:stack");
        const std::string report = check(buffer, buffer + 8, 8);
        __ct_stack_pop(depth);
        EXPECT_EQ(report, "");
    }

    // Shadow memory describes heap blocks only: a stack object is checked against its
    // own bounds.
    TEST_F(BoundsCheck, ShadowModeChecksAStackObjectAgainstItsBounds)
    {
        ct_set_enabled(CT_FEATURE_SHADOW, 1);
        char buffer[16] = {};
        const size_t depth = __ct_stack_push(buffer, sizeof(buffer), "test:stack");
        const std::string inside = check(buffer, buffer + 8, 8);
        const std::string past = check(buffer, buffer + 16, 1);
        __ct_stack_pop(depth);
        EXPECT_EQ(inside, "");
        EXPECT_NE(past.find("stack-buffer-overflow"), std::string::npos) << past;
    }

    // As for heap blocks, only aggressive mode looks for the object containing an
    // interior base.
    TEST_F(BoundsCheck, AggressiveModeFindsTheStackObjectContainingAnInteriorBase)
    {
        char buffer[16] = {};
        const size_t depth = __ct_stack_push(buffer, sizeof(buffer), "test:stack");
        const std::string defaultMode = check(buffer + 4, buffer + 16, 1);
        ct_set_enabled(CT_FEATURE_SHADOW, 1);
        ct_set_enabled(CT_FEATURE_SHADOW_AGGR, 1);
        const std::string aggressiveMode = check(buffer + 4, buffer + 16, 1);
        __ct_stack_pop(depth);
        EXPECT_EQ(defaultMode, "");
        EXPECT_NE(aggressiveMode.find("stack-buffer-overflow"), std::string::npos)
            << aggressiveMode;
    }

    TEST_F(BoundsCheck, ShadowModeAcceptsAnAccessInsideAKnownAllocation)
    {
        ct_set_enabled(CT_FEATURE_SHADOW, 1);
        char* block = allocate(5);
        markValid(block, kRequested);
        EXPECT_EQ(check(block, block + 4, 4), "");
    }
} // namespace
