// SPDX-License-Identifier: Apache-2.0
//
// The logger calls the runtime's sink without holding a lock of its own: the sink may take the
// runtime's log lock, and the logger's fork() handlers and the runtime's take their locks in an
// order neither controls, so a thread holding a lock of one while it waits for a lock of the
// other could deadlock with a fork(). runtime_table_full_test.cpp checks the other direction.
// POSIX only, where both register fork handlers.
//
// The probe replaces the runtime's sink while lines are logged and restores it after.
#include "ct_runtime_internal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string_view>
#include <thread>

namespace
{
    using namespace std::chrono_literals;

    // Two threads log at once; the first stays in the sink until the second's line arrives. The
    // logger must not call the sink under its output lock, or the second line would wait for
    // the first to leave the sink.
    std::atomic<int> lines_in_sink{0};
    std::atomic<bool> second_line_arrived_meanwhile{false};

    void concurrencyProbe(const char* data, size_t size)
    {
        const std::string_view line(data, size);
        if (line.find("probe-first") != std::string_view::npos)
        {
            lines_in_sink.fetch_add(1);
            const auto deadline = std::chrono::steady_clock::now() + 1s;
            while (lines_in_sink.load() < 2 && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::yield();
            }
            second_line_arrived_meanwhile.store(lines_in_sink.load() >= 2);
        }
        else if (line.find("probe-second") != std::string_view::npos)
        {
            lines_in_sink.fetch_add(1);
        }
    }

    TEST(LogLocks, TheLoggerCallsTheRuntimeSinkWithoutItsOutputLock)
    {
        lines_in_sink = 0;
        second_line_arrived_meanwhile = false;
        coretrace::set_sink(concurrencyProbe);
        std::thread first([] { ct_log(CTLevel::Info, "probe-first\n"); });
        while (lines_in_sink.load() < 1)
        {
            std::this_thread::yield();
        }
        std::thread second([] { ct_log(CTLevel::Info, "probe-second\n"); });
        first.join();
        second.join();
        coretrace::set_sink(ct_log_sink);
        EXPECT_TRUE(second_line_arrived_meanwhile.load())
            << "the second line waited for the first to leave the sink";
    }
} // namespace
