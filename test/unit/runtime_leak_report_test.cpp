// SPDX-License-Identifier: Apache-2.0
//
// The leak report of the Windows runtime, which runs at process exit when ExitProcess may
// have killed a thread that held the allocation table (#187).
#include "ct_runtime_internal.h"

#include <gtest/gtest.h>

#if defined(_WIN32)
#include <chrono>
#include <future>
#include <thread>

namespace
{
    // Another thread holds the table: the report returns at once, skipped, instead of
    // waiting for it; once the table is free, it runs.
    TEST(LeakReport, SkipsWithoutWaitingWhenAnotherThreadHoldsTheTable)
    {
        std::promise<void> held;
        std::promise<void> release;
        std::thread holder(
            [&]
            {
                ct_lock_acquire();
                held.set_value();
                release.get_future().wait();
                ct_lock_release();
            });
        held.get_future().wait();

        std::future<bool> report = std::async(std::launch::async, [] { return ct_report_leaks(); });
        const bool returned =
            report.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
        release.set_value();
        holder.join();

        EXPECT_TRUE(returned) << "the report waited for the table";
        EXPECT_FALSE(report.get());
        EXPECT_TRUE(ct_report_leaks());
    }
} // namespace
#endif
