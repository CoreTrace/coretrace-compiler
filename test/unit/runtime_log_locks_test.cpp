// SPDX-License-Identifier: Apache-2.0
//
// The logger calls the runtime's sink without holding a lock of its own: the sink may take the
// runtime's log lock, and the logger's fork() handlers and the runtime's take their locks in an
// order neither controls, so a thread holding a lock of one while it waits for a lock of the
// other could deadlock with a fork(). runtime_table_full_test.cpp checks the other direction,
// and runtime_log_fallback_test.cpp the sink's fallback.
//
// With the logger's own serialisation off, the runtime serialises every line it writes: the
// writers that do not go through ct_log() open a CtLogBlock too, and wait for the log lock.
// POSIX only, where both register fork handlers.
//
// The probes replace the runtime's sink while lines are logged and restore it after, or point
// stderr at a pipe nobody reads until asked.
#include "ct_runtime_internal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

#include <poll.h>
#include <unistd.h>

extern "C"
{
    void __ct_trace_enter(const char* func);
}

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

    // Stderr on a pipe nobody reads until asked, for the life of the object.
    class UnreadStderrPipe
    {
      public:
        UnreadStderrPipe()
        {
            int fds[2];
            EXPECT_EQ(pipe(fds), 0);
            read_end_ = fds[0];
            saved_ = dup(2);
            EXPECT_EQ(dup2(fds[1], 2), 2);
            close(fds[1]);
        }
        ~UnreadStderrPipe()
        {
            dup2(saved_, 2);
            close(saved_);
            close(read_end_);
        }
        UnreadStderrPipe(const UnreadStderrPipe&) = delete;
        UnreadStderrPipe& operator=(const UnreadStderrPipe&) = delete;

        // What was written since the last call, without waiting for more.
        std::string take()
        {
            std::string text;
            char buffer[4096];
            pollfd ready{read_end_, POLLIN, 0};
            while (poll(&ready, 1, 0) > 0 && (ready.revents & POLLIN) != 0)
            {
                const ssize_t got = read(read_end_, buffer, sizeof(buffer));
                if (got <= 0)
                {
                    break;
                }
                text.append(buffer, static_cast<size_t>(got));
            }
            return text;
        }

      private:
        int read_end_ = -1;
        int saved_ = -1;
    };

    // Runs `write` on another thread while this one holds the log lock: nothing reaches stderr
    // until the lock is released, then `expected`.
    template <typename Write>
    void expectWrittenUnderTheLogLock(Write write, std::string_view expected)
    {
        UnreadStderrPipe stderr_pipe;
        std::atomic<bool> writing{false};
        ct_log_lock_acquire();
        std::thread writer(
            [&]
            {
                writing.store(true);
                write();
            });
        while (!writing.load())
        {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(200ms);
        const std::string while_held = stderr_pipe.take();
        ct_log_lock_release();
        writer.join();
        const std::string after = stderr_pipe.take();

        EXPECT_EQ(while_held, "") << "written without waiting for the log lock";
        EXPECT_NE(after.find(expected), std::string::npos) << after;
    }

    TEST(LogLocks, TheEarlyTraceWaitsForTheLogLock)
    {
        const uint64_t saved = ct_get_features();
        ct_set_enabled(CT_FEATURE_TRACE, 1);
        ct_set_enabled(CT_FEATURE_EARLY_TRACE, 1);
        expectWrittenUnderTheLogLock([] { __ct_trace_enter("probe_early_trace"); },
                                     "ct: enter probe_early_trace\n");
        ct_set_enabled(CT_FEATURE_TRACE, (saved & CT_FEATURE_TRACE) != 0);
        ct_set_enabled(CT_FEATURE_EARLY_TRACE, (saved & CT_FEATURE_EARLY_TRACE) != 0);
    }

    TEST(LogLocks, TheFormatErrorFallbackWaitsForTheLogLock)
    {
        // Two replacement fields for one argument: formatting the line throws.
        expectWrittenUnderTheLogLock([] { ct_log(CTLevel::Info, "{} {}\n", 1); },
                                     "ct: log format error\n");
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
