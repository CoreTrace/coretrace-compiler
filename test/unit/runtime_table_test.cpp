// SPDX-License-Identifier: Apache-2.0
//
// The allocation table is process-global; every test releases what it inserts so
// the leak report at exit stays empty and tests do not observe each other.
#include "ct_runtime_internal.h"

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
    }

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
    }
} // namespace
