// SPDX-License-Identifier: Apache-2.0
//
// The allocation table is process-global; every test releases what it inserts so
// the leak report at exit stays empty and tests do not observe each other.
#include "ct_runtime_block_index.h"
#include "ct_runtime_internal.h"
#include "ct_runtime_quarantine.h"
#if !defined(_WIN32)
#include "ct_runtime_alloc_internal.h"
#endif

#include <gtest/gtest.h>

#include <cstdint>

// The compiler emits these configuration globals into every instrumented module
// (see emitRuntimeConfigGlobals); the runtime imports them weakly. Define them the
// way an instrumented program with default options would.
extern "C"
{
    int __ct_config_shadow = 0;
    int __ct_config_shadow_aggressive = 0;
    int __ct_config_bounds_no_abort = 0;
    int __ct_config_disable_alloc = 0;
    int __ct_config_disable_autofree = 1;
    int __ct_config_disable_alloc_trace = 0;
    int __ct_config_vtable_diag = 0;
}

namespace
{
    // Fake, distinct, 16-byte aligned addresses; nothing is dereferenced.
    void* fakeAddress(uintptr_t index, uintptr_t base = 0x10000000u)
    {
        return reinterpret_cast<void*>(base + index * 16u);
    }

    struct TableLock
    {
        TableLock()
        {
            ct_lock_acquire();
        }
        ~TableLock()
        {
            ct_lock_release();
        }
    };

    constexpr unsigned char kKindMalloc = 0;

    TEST(AllocTable, InsertLookupRemoveRoundTrip)
    {
        TableLock lock;
        void* ptr = fakeAddress(1, 0x20000000u);
        ASSERT_EQ(ct_table_insert(ptr, 24, 32, "site:1", kKindMalloc), 1);

        size_t size = 0;
        size_t req = 0;
        const char* site = nullptr;
        unsigned char state = 0;
        ASSERT_EQ(ct_table_lookup(ptr, &size, &req, &site, &state), 1);
        EXPECT_EQ(size, 32u);
        EXPECT_EQ(req, 24u);
        EXPECT_STREQ(site, "site:1");
        EXPECT_EQ(state, CT_ENTRY_USED);

        size = req = 0;
        site = nullptr;
        ASSERT_EQ(ct_table_remove(ptr, &size, &req, &site), 1);
        EXPECT_EQ(size, 32u);
        EXPECT_EQ(req, 24u);
        EXPECT_STREQ(site, "site:1");
        ct_table_forget_freed(ptr);
    }

    TEST(AllocTable, RemovedEntryStaysVisibleAsFreed)
    {
        TableLock lock;
        void* ptr = fakeAddress(2, 0x20000000u);
        ASSERT_EQ(ct_table_insert(ptr, 8, 16, "site:2", kKindMalloc), 1);
        ASSERT_EQ(ct_table_remove(ptr, nullptr, nullptr, nullptr), 1);

        unsigned char state = 0;
        ASSERT_EQ(ct_table_lookup(ptr, nullptr, nullptr, nullptr, &state), 1);
        EXPECT_EQ(state, CT_ENTRY_FREED);
        // A second release of the same pointer is not a live removal.
        EXPECT_NE(ct_table_remove(ptr, nullptr, nullptr, nullptr), 1);
        ct_table_forget_freed(ptr);
    }

    TEST(AllocTable, UnknownPointerIsNotFound)
    {
        TableLock lock;
        void* ptr = fakeAddress(3, 0x20000000u);
        EXPECT_EQ(ct_table_lookup(ptr, nullptr, nullptr, nullptr, nullptr), 0);
        EXPECT_EQ(ct_table_remove(ptr, nullptr, nullptr, nullptr), 0);
    }

    TEST(AllocTable, InteriorPointerResolvesToItsBlock)
    {
        TableLock lock;
        void* ptr = fakeAddress(4, 0x20000000u);
        ASSERT_EQ(ct_table_insert(ptr, 100, 112, "site:4", kKindMalloc), 1);

        void* base = nullptr;
        size_t size = 0;
        unsigned char state = 0;
        const void* inside = static_cast<const char*>(ptr) + 50;
        ASSERT_EQ(ct_table_lookup_containing(inside, &base, &size, nullptr, nullptr, &state), 1);
        EXPECT_EQ(base, ptr);
        EXPECT_EQ(size, 112u);
        EXPECT_EQ(state, CT_ENTRY_USED);

        const void* pastEnd = static_cast<const char*>(ptr) + 112;
        EXPECT_EQ(ct_table_lookup_containing(pastEnd, nullptr, nullptr, nullptr, nullptr, nullptr),
                  0);

        ASSERT_EQ(ct_table_remove(ptr, nullptr, nullptr, nullptr), 1);
        ct_table_forget_freed(ptr);
    }

    TEST(AllocTable, ReinsertingAFreedAddressMakesItLiveAgain)
    {
        TableLock lock;
        void* ptr = fakeAddress(5, 0x20000000u);
        ASSERT_EQ(ct_table_insert(ptr, 8, 16, "first", kKindMalloc), 1);
        ASSERT_EQ(ct_table_remove(ptr, nullptr, nullptr, nullptr), 1);
        ASSERT_EQ(ct_table_insert(ptr, 40, 48, "second", kKindMalloc), 1);

        size_t size = 0;
        const char* site = nullptr;
        unsigned char state = 0;
        ASSERT_EQ(ct_table_lookup(ptr, &size, nullptr, &site, &state), 1);
        EXPECT_EQ(state, CT_ENTRY_USED);
        EXPECT_EQ(size, 48u);
        EXPECT_STREQ(site, "second");
        ASSERT_EQ(ct_table_remove(ptr, nullptr, nullptr, nullptr), 1);
        ct_table_forget_freed(ptr);
    }

#if !defined(_WIN32)
    // The POSIX table is open-addressed; the Windows one is keyed by address, so no insert
    // can land on another address's record there.
    //
    // A block recorded as freed is in the quarantine, still allocated: an insert for another
    // address probing through its slot must not take it, or an access to the block would
    // no longer be reported as a use-after-free.
    TEST(AllocTable, InsertKeepsTheRecordOfAFreedBlock)
    {
        TableLock lock;
        void* freed = fakeAddress(1, 0x50000000u);
        const size_t home = ct_hash_ptr(freed, ct_alloc_table_mask);
        void* other = nullptr;
        for (uintptr_t i = 2; !other && i < 1000000; ++i)
        {
            if (ct_hash_ptr(fakeAddress(i, 0x50000000u), ct_alloc_table_mask) == home)
            {
                other = fakeAddress(i, 0x50000000u);
            }
        }
        ASSERT_NE(other, nullptr);

        ASSERT_EQ(ct_table_insert(freed, 16, 16, "freed", kKindMalloc), 1);
        ASSERT_EQ(ct_table_remove(freed, nullptr, nullptr, nullptr), 1);
        ASSERT_EQ(ct_table_insert(other, 16, 16, "other", kKindMalloc), 1);

        unsigned char state = 0;
        EXPECT_EQ(ct_table_lookup(freed, nullptr, nullptr, nullptr, &state), 1);
        EXPECT_EQ(state, CT_ENTRY_FREED);

        ASSERT_EQ(ct_table_remove(other, nullptr, nullptr, nullptr), 1);
        ct_table_forget_freed(other);
        ct_table_forget_freed(freed);
    }
#endif

    TEST(AllocTable, GrowsPastTheInitialCapacityAndKeepsEveryEntry)
    {
        // The initial table holds 2^16 entries; this exceeds it, as
        // test/ct_alloc_growth.c does end to end.
        constexpr uintptr_t kCount = 70000;
        TableLock lock;
        for (uintptr_t i = 0; i < kCount; ++i)
        {
            ASSERT_EQ(ct_table_insert(fakeAddress(i, 0x40000000u), 16, 16, "growth", kKindMalloc),
                      1)
                << "insert " << i;
        }
        for (uintptr_t i = 0; i < kCount; ++i)
        {
            unsigned char state = 0;
            ASSERT_EQ(
                ct_table_lookup(fakeAddress(i, 0x40000000u), nullptr, nullptr, nullptr, &state), 1)
                << "lookup " << i;
            EXPECT_EQ(state, CT_ENTRY_USED);
        }
        for (uintptr_t i = 0; i < kCount; ++i)
        {
            ASSERT_EQ(ct_table_remove(fakeAddress(i, 0x40000000u), nullptr, nullptr, nullptr), 1)
                << "remove " << i;
        }
        for (uintptr_t i = 0; i < kCount; ++i)
        {
            ct_table_forget_freed(fakeAddress(i, 0x40000000u));
        }
    }
    // The containing lookup, with the index behind it (#185). Each test uses addresses of its
    // own, above every other test's, and leaves no live block.
    struct Containing
    {
        bool found = false;
        void* base = nullptr;
        size_t size = 0;
        unsigned char state = 0;
    };

    Containing lookupContaining(uintptr_t address)
    {
        Containing result;
        result.found =
            ct_table_lookup_containing(reinterpret_cast<const void*>(address), &result.base,
                                       &result.size, nullptr, nullptr, &result.state) == 1;
        return result;
    }

    TEST(AllocTable, ContainingLookupSeesAQuarantinedBlock)
    {
        TableLock lock;
        void* ptr = fakeAddress(0, 0x70000000u);
        ASSERT_EQ(ct_table_insert(ptr, 200, 208, "quarantined", kKindMalloc), 1);
        ASSERT_EQ(ct_table_remove(ptr, nullptr, nullptr, nullptr), 1);

        const Containing inside = lookupContaining(0x70000000u + 100);
        ASSERT_TRUE(inside.found);
        EXPECT_EQ(inside.base, ptr);
        EXPECT_EQ(inside.size, 208u);
        EXPECT_EQ(inside.state, CT_ENTRY_FREED);
        ct_table_forget_freed(ptr);
    }

    TEST(AllocTable, ContainingLookupForgetsABlockReturnedToTheAllocator)
    {
        TableLock lock;
        void* ptr = fakeAddress(0, 0x70100000u);
        ASSERT_EQ(ct_table_insert(ptr, 64, 64, "returned", kKindMalloc), 1);
        ASSERT_EQ(ct_table_remove(ptr, nullptr, nullptr, nullptr), 1);
        ct_table_forget_freed(ptr);

        EXPECT_FALSE(lookupContaining(0x70100000u + 8).found);
    }

    TEST(AllocTable, ContainingLookupFollowsTheReuseOfAnAddress)
    {
        TableLock lock;
        void* ptr = fakeAddress(0, 0x70200000u);
        // Returned to the allocator, then handed out again, smaller.
        ASSERT_EQ(ct_table_insert(ptr, 256, 256, "before", kKindMalloc), 1);
        ASSERT_EQ(ct_table_remove(ptr, nullptr, nullptr, nullptr), 1);
        ct_table_forget_freed(ptr);
        ASSERT_EQ(ct_table_insert(ptr, 32, 32, "after", kKindMalloc), 1);

        const Containing inside = lookupContaining(0x70200000u + 16);
        ASSERT_TRUE(inside.found);
        EXPECT_EQ(inside.size, 32u);
        EXPECT_EQ(inside.state, CT_ENTRY_USED);
        EXPECT_FALSE(lookupContaining(0x70200000u + 100).found);

        ASSERT_EQ(ct_table_remove(ptr, nullptr, nullptr, nullptr), 1);
        ct_table_forget_freed(ptr);
    }

    TEST(AllocTable, ContainingLookupFollowsAFreedAddressHandedOutAgain)
    {
        TableLock lock;
        void* ptr = fakeAddress(0, 0x70300000u);
        // Its freed record is taken over by the new block, of another size.
        ASSERT_EQ(ct_table_insert(ptr, 256, 256, "before", kKindMalloc), 1);
        ASSERT_EQ(ct_table_remove(ptr, nullptr, nullptr, nullptr), 1);
        ASSERT_EQ(ct_table_insert(ptr, 32, 32, "after", kKindMalloc), 1);

        const Containing inside = lookupContaining(0x70300000u + 16);
        ASSERT_TRUE(inside.found);
        EXPECT_EQ(inside.size, 32u);
        EXPECT_EQ(inside.state, CT_ENTRY_USED);
        EXPECT_FALSE(lookupContaining(0x70300000u + 100).found);

        ASSERT_EQ(ct_table_remove(ptr, nullptr, nullptr, nullptr), 1);
        ct_table_forget_freed(ptr);
    }

    TEST(AllocTable, ContainingLookupFollowsTheSizeOfALiveBlock)
    {
        TableLock lock;
        void* ptr = fakeAddress(0, 0x70400000u);
        ASSERT_EQ(ct_table_insert(ptr, 64, 64, "small", kKindMalloc), 1);
        ASSERT_EQ(ct_table_insert(ptr, 4096, 4096, "large", kKindMalloc), 1);

        const Containing inside = lookupContaining(0x70400000u + 1000);
        ASSERT_TRUE(inside.found);
        EXPECT_EQ(inside.size, 4096u);

        ASSERT_EQ(ct_table_remove(ptr, nullptr, nullptr, nullptr), 1);
        ct_table_forget_freed(ptr);
    }

    TEST(AllocTable, ContainingLookupFindsEveryBlockAcrossTheTableGrowth)
    {
        // More blocks than the initial table holds, live and freed.
        constexpr uintptr_t kCount = 70000;
        constexpr uintptr_t kBase = 0x71000000u;
        TableLock lock;
        for (uintptr_t i = 0; i < kCount; ++i)
        {
            ASSERT_EQ(ct_table_insert(reinterpret_cast<void*>(kBase + i * 64), 48, 48, "grown",
                                      kKindMalloc),
                      1);
        }
        for (uintptr_t i = 0; i < kCount; i += 2)
        {
            ASSERT_EQ(
                ct_table_remove(reinterpret_cast<void*>(kBase + i * 64), nullptr, nullptr, nullptr),
                1);
        }
        for (uintptr_t i = 0; i < kCount; i += 997)
        {
            const Containing inside = lookupContaining(kBase + i * 64 + 47);
            ASSERT_TRUE(inside.found) << i;
            EXPECT_EQ(inside.base, reinterpret_cast<void*>(kBase + i * 64));
            EXPECT_EQ(inside.state, i % 2 == 0 ? CT_ENTRY_FREED : CT_ENTRY_USED);
            EXPECT_FALSE(lookupContaining(kBase + i * 64 + 48).found) << i;
        }
        for (uintptr_t i = 0; i < kCount; ++i)
        {
            void* ptr = reinterpret_cast<void*>(kBase + i * 64);
            if (i % 2 == 1)
            {
                ASSERT_EQ(ct_table_remove(ptr, nullptr, nullptr, nullptr), 1);
            }
            ct_table_forget_freed(ptr);
        }
    }

    // When the index cannot grow, the lookup passes over the table instead, and finds a block
    // the index lacks, live then freed. The test lowers the index's maximum capacity to the
    // capacity it has, and fills it until a block does not fit.
    TEST(AllocTable, ContainingLookupScansTheTableWhenTheIndexCannotGrow)
    {
        constexpr uintptr_t kFillerBase = 0x72000000u;
        constexpr uintptr_t kStride = 64;
        void* const beyond = reinterpret_cast<void*>(0x73000000u);
        const char* const beyond_site = "beyond the index";
        TableLock lock;
        ct_block_index* index = ct_alloc_index();
        ASSERT_FALSE(index->incomplete);
        const size_t max_capacity = index->max_capacity;
        index->max_capacity = index->capacity;

        uintptr_t fillers = 0;
        while (!index->incomplete)
        {
            ASSERT_LE(fillers, index->capacity) << "the index kept growing";
            ASSERT_EQ(ct_table_insert(reinterpret_cast<void*>(kFillerBase + fillers * kStride), 40,
                                      48, "filler", kKindMalloc),
                      1);
            ++fillers;
        }
        ASSERT_EQ(ct_table_insert(beyond, 100, 112, beyond_site, kKindMalloc), 1);
        uintptr_t indexed_base = 0;
        size_t indexed_size = 0;
        ASSERT_FALSE(ct_block_index_find(index, reinterpret_cast<uintptr_t>(beyond) + 8,
                                         &indexed_base, &indexed_size))
            << "the block is in the index, which then does not test the fallback";

        for (const unsigned char state : {CT_ENTRY_USED, CT_ENTRY_FREED})
        {
            if (state == CT_ENTRY_FREED)
            {
                ASSERT_EQ(ct_table_remove(beyond, nullptr, nullptr, nullptr), 1);
            }
            void* base = nullptr;
            size_t size = 0;
            size_t req_size = 0;
            const char* site = nullptr;
            unsigned char found_state = 0;
            ASSERT_EQ(ct_table_lookup_containing(static_cast<char*>(beyond) + 100, &base, &size,
                                                 &req_size, &site, &found_state),
                      1)
                << "state " << static_cast<int>(state);
            EXPECT_EQ(base, beyond);
            EXPECT_EQ(size, 112u);
            EXPECT_EQ(req_size, 100u);
            EXPECT_STREQ(site, beyond_site);
            EXPECT_EQ(found_state, state);
            EXPECT_EQ(ct_table_lookup_containing(static_cast<char*>(beyond) + 112, nullptr, nullptr,
                                                 nullptr, nullptr, nullptr),
                      0);
        }
        // The filler that did not fit either.
        const Containing last = lookupContaining(kFillerBase + (fillers - 1) * kStride + 47);
        ASSERT_TRUE(last.found);
        EXPECT_EQ(last.base, reinterpret_cast<void*>(kFillerBase + (fillers - 1) * kStride));
        EXPECT_EQ(last.state, CT_ENTRY_USED);

        ct_table_forget_freed(beyond);
        for (uintptr_t i = 0; i < fillers; ++i)
        {
            void* ptr = reinterpret_cast<void*>(kFillerBase + i * kStride);
            ASSERT_EQ(ct_table_remove(ptr, nullptr, nullptr, nullptr), 1);
            ct_table_forget_freed(ptr);
        }
        // The blocks the index lacked have left the table: it holds every block the table
        // records again, and the tests after this one go through it.
        index->max_capacity = max_capacity;
        index->incomplete = false;
    }
} // namespace
