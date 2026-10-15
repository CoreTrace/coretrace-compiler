// SPDX-License-Identifier: Apache-2.0
//
// The allocation table found full is reported once the allocation lock is released: logging
// takes the logger's locks, which the logger's fork() handlers take, in an order relative to the
// runtime's handlers, which take the allocation lock, that neither controls. A thread holding
// the allocation lock while it waits for the logger could deadlock with a fork(). POSIX only,
// where both register fork handlers.
//
// An executable of its own: a table filled up keeps its tombstones once emptied, so that every
// later insert probes the whole table, which would slow the other runtime tests down to a halt
// in a process they share. The runtime also warns only once per process, so each test fills
// the table in a process of its own, as ctest runs them; run together, the second one skips.
// The probe replaces the runtime's sink while the warning is logged and restores it after.
#include "ct_runtime_alloc_internal.h"
#include "ct_runtime_internal.h"
#include "ct_runtime_quarantine.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <string_view>
#include <thread>
#include <vector>

// The configuration globals of an instrumented program with default options, as
// runtime_table_test.cpp defines them.
extern "C"
{
    int __ct_config_shadow = 0;
    int __ct_config_shadow_aggressive = 0;
    int __ct_config_bounds_no_abort = 0;
    int __ct_config_disable_alloc = 0;
    int __ct_config_disable_autofree = 1;
    int __ct_config_disable_alloc_trace = 0;
    int __ct_config_vtable_diag = 0;

    void* __ct_sbrk(size_t incr, const char* site);
}

namespace
{
    using namespace std::chrono_literals;

    // True when another thread takes and releases the allocation lock within a second: the
    // lock is not held, or not held for long.
    bool allocationLockIsFree(std::thread& taker)
    {
        std::promise<void> taken;
        std::future<void> done = taken.get_future();
        taker = std::thread(
            [taken = std::move(taken)]() mutable
            {
                ct_lock_acquire();
                ct_lock_release();
                taken.set_value();
            });
        return done.wait_for(1s) == std::future_status::ready;
    }

    // What the probe sink saw of the "alloc table full" warning.
    std::atomic<bool> warned{false};
    std::atomic<bool> lock_free_while_warning{false};
    std::thread lock_taker;

    void tableFullProbe(const char* data, size_t size)
    {
        if (std::string_view(data, size).find("alloc table full") == std::string_view::npos)
        {
            return;
        }
        warned.store(true);
        lock_free_while_warning.store(allocationLockIsFree(lock_taker));
    }

    // Fills the allocation table with fake blocks until it takes no more; returns them.
    std::vector<void*> fillTheAllocationTable()
    {
        std::vector<void*> blocks;
        ct_lock_acquire();
        for (uintptr_t i = 0;; ++i)
        {
            void* block = reinterpret_cast<void*>(uintptr_t{0x7f0000000000} + i * 16);
            if (!ct_table_insert(block, 16, 16, "full:fill", 0))
            {
                break;
            }
            blocks.push_back(block);
        }
        ct_lock_release();
        return blocks;
    }

    void emptyTheAllocationTable(const std::vector<void*>& blocks)
    {
        ct_lock_acquire();
        for (void* block : blocks)
        {
            (void)ct_table_remove(block, nullptr, nullptr, nullptr);
            ct_table_forget_freed(block);
        }
        ct_lock_release();
    }

    // Whether a test of this process filled the table.
    bool table_filled_in_this_process = false;

    // Runs `allocate` with the table full and the probe sink, then checks the warning.
    template <typename Allocate> void expectTheTableFullWarningOutsideTheLock(Allocate allocate)
    {
        if (table_filled_in_this_process)
        {
            GTEST_SKIP() << "the table was filled earlier in this process";
        }
        table_filled_in_this_process = true;
        const std::vector<void*> blocks = fillTheAllocationTable();
        ASSERT_FALSE(blocks.empty());
        warned = false;
        lock_free_while_warning = false;

        coretrace::set_sink(tableFullProbe);
        allocate();
        coretrace::set_sink(ct_log_sink);
        if (lock_taker.joinable())
        {
            lock_taker.join();
        }
        emptyTheAllocationTable(blocks);

        ASSERT_TRUE(warned.load()) << "no table-full warning";
        EXPECT_TRUE(lock_free_while_warning.load())
            << "the warning was logged with the allocation lock held";
    }

    TEST(AllocTableFull, AnAllocationTheTableIsTooFullForIsReportedOutsideTheLock)
    {
        expectTheTableFullWarningOutsideTheLock(
            []
            {
                ct_track_allocation(reinterpret_cast<void*>(uintptr_t{0x7e0000000000}), 16,
                                    "full:track", 0);
            });
    }

#if defined(__linux__)
    // The runtime records the blocks of sbrk() on Linux only.
    TEST(AllocTableFull, AnSbrkTheTableIsTooFullForIsReportedOutsideTheLock)
    {
        expectTheTableFullWarningOutsideTheLock(
            []
            {
                void* previous = __ct_sbrk(16, "full:sbrk");
                ASSERT_NE(previous, reinterpret_cast<void*>(-1));
                (void)__ct_sbrk(static_cast<size_t>(-16), "full:sbrk");
            });
    }
#endif

} // namespace
