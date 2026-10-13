// SPDX-License-Identifier: Apache-2.0
//
// What the runtime's log lines become on stderr: every byte written, in order, when the
// descriptor takes them in pieces, and the lines of a box, or of any CtLogBlock, written
// together, unchanged, when other threads log at the same time. Each test points stderr at
// a pipe of its own and restores it.
#include "ct_runtime_internal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <io.h>
#endif

namespace
{
#if defined(_WIN32)
    int ct_test_pipe(int fds[2])
    {
        return _pipe(fds, 1 << 16, _O_BINARY);
    }
    int ct_test_dup(int fd)
    {
        return _dup(fd);
    }
    // As POSIX dup2: `to` on success. _dup2 returns 0.
    int ct_test_dup2(int from, int to)
    {
        return _dup2(from, to) == 0 ? to : -1;
    }
    int ct_test_close(int fd)
    {
        return _close(fd);
    }
    long ct_test_read(int fd, char* buffer, unsigned size)
    {
        return _read(fd, buffer, size);
    }
#else
    int ct_test_pipe(int fds[2])
    {
        return pipe(fds);
    }
    int ct_test_dup(int fd)
    {
        return dup(fd);
    }
    int ct_test_dup2(int from, int to)
    {
        return dup2(from, to);
    }
    int ct_test_close(int fd)
    {
        return close(fd);
    }
    long ct_test_read(int fd, char* buffer, unsigned size)
    {
        return static_cast<long>(read(fd, buffer, size));
    }
#endif

    // Stderr on a pipe for the life of the object; a thread reads it, after `delay`.
    class StderrPipe
    {
      public:
        explicit StderrPipe(bool non_blocking = false,
                            std::chrono::milliseconds delay = std::chrono::milliseconds(0))
        {
            int fds[2];
            EXPECT_EQ(ct_test_pipe(fds), 0);
            read_end_ = fds[0];
#if !defined(_WIN32)
            if (non_blocking)
            {
                EXPECT_EQ(fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK), 0);
            }
#else
            (void)non_blocking;
#endif
            saved_ = ct_test_dup(2);
            EXPECT_EQ(ct_test_dup2(fds[1], 2), 2);
            ct_test_close(fds[1]);
            reader_ = std::thread(
                [this, delay]
                {
                    std::this_thread::sleep_for(delay);
                    char buffer[4096];
                    long got = 0;
                    while ((got = ct_test_read(read_end_, buffer, sizeof(buffer))) > 0)
                    {
                        text_.append(buffer, static_cast<size_t>(got));
                    }
                });
        }

        // Restores stderr and returns all that was written to the pipe.
        std::string finish()
        {
            ct_test_dup2(saved_, 2);
            ct_test_close(saved_);
            reader_.join();
            ct_test_close(read_end_);
            return text_;
        }

      private:
        int read_end_ = -1;
        int saved_ = -1;
        std::thread reader_;
        std::string text_;
    };

    std::vector<std::string> splitLines(const std::string& text)
    {
        std::vector<std::string> lines;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line))
        {
            lines.push_back(line);
        }
        return lines;
    }

#if !defined(_WIN32)
    // A non-blocking pipe that nobody reads for a while fills up: write() takes part of
    // what it is given, then refuses with EAGAIN until the reader catches up.
    TEST(LogOutput, KeepsEveryLineWhenStderrTakesThemInPieces)
    {
        constexpr int kLines = 4000;
        StderrPipe pipe(true, std::chrono::milliseconds(200));
        for (int i = 0; i < kLines; ++i)
        {
            ct_log(CTLevel::Info, "line {} of the partial-write test, padded to fill the pipe\n",
                   i);
        }
        const std::vector<std::string> lines = splitLines(pipe.finish());

        ASSERT_EQ(lines.size(), static_cast<size_t>(kLines));
        for (int i = 0; i < kLines; ++i)
        {
            const std::string expected = "line " + std::to_string(i) + " of the partial-write test";
            ASSERT_NE(lines[i].find(expected), std::string::npos) << lines[i];
        }
    }
#endif

#if !defined(_WIN32)
    // What the crash handler writes: on a full non-blocking stderr, the writer gives up rather
    // than wait for a reader, so that the process can end. Other writes wait.
    TEST(LogOutput, AWriteThatMustNotWaitGivesUpOnAFullStderr)
    {
        int fds[2];
        ASSERT_EQ(pipe(fds), 0);
        ASSERT_EQ(fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK), 0);
        ASSERT_EQ(fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK), 0);
        const int saved = dup(2);
        ASSERT_EQ(dup2(fds[1], 2), 2);
        close(fds[1]);
        char block[4096];
        std::memset(block, 'x', sizeof(block));
        while (write(2, block, sizeof(block)) > 0)
        {
        }

        std::promise<void> done;
        std::future<void> finished = done.get_future();
        std::thread writer(
            [&done]
            {
                ct_write_stderr("report\n", 7, CtStderrFull::GiveUp);
                done.set_value();
            });
        const bool returned =
            finished.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        // A writer that waits ends once the pipe is read: the test ends either way.
        while (finished.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready)
        {
            while (read(fds[0], block, sizeof(block)) > 0)
            {
            }
        }
        writer.join();
        dup2(saved, 2);
        close(saved);
        close(fds[0]);
        EXPECT_TRUE(returned) << "the write waited for stderr to be read";
    }
#endif

    // Stderr on a pipe nobody reads until asked: for lines that fit in it.
    class UnreadStderrPipe
    {
      public:
        UnreadStderrPipe()
        {
            int fds[2];
            EXPECT_EQ(ct_test_pipe(fds), 0);
            read_end_ = fds[0];
            saved_ = ct_test_dup(2);
            EXPECT_EQ(ct_test_dup2(fds[1], 2), 2);
            ct_test_close(fds[1]);
        }
        ~UnreadStderrPipe()
        {
            ct_test_dup2(saved_, 2);
            ct_test_close(saved_);
            ct_test_close(read_end_);
        }
        UnreadStderrPipe(const UnreadStderrPipe&) = delete;
        UnreadStderrPipe& operator=(const UnreadStderrPipe&) = delete;

        // What was written since the last call, without waiting for more.
        std::string take()
        {
            std::string text;
            char buffer[4096];
            while (pending())
            {
                const long got = ct_test_read(read_end_, buffer, sizeof(buffer));
                if (got <= 0)
                {
                    break;
                }
                text.append(buffer, static_cast<size_t>(got));
            }
            return text;
        }

      private:
        bool pending() const
        {
#if defined(_WIN32)
            DWORD available = 0;
            return PeekNamedPipe(reinterpret_cast<HANDLE>(_get_osfhandle(read_end_)), nullptr, 0,
                                 nullptr, &available, nullptr) &&
                   available > 0;
#else
            pollfd ready{read_end_, POLLIN, 0};
            return poll(&ready, 1, 0) > 0 && (ready.revents & POLLIN) != 0;
#endif
        }

        int read_end_ = -1;
        int saved_ = -1;
    };

    TEST(LogBlock, WritesItsLinesWhenItEnds)
    {
        UnreadStderrPipe pipe;
        {
            CtLogBlock block;
            ct_log(CTLevel::Info, "first line of the block\n");
            ct_log(CTLevel::Warn, "second line of the block\n");
            EXPECT_EQ(pipe.take(), "");
        }
        const std::vector<std::string> lines = splitLines(pipe.take());
        ASSERT_EQ(lines.size(), 2u);
        EXPECT_NE(lines[0].find("[INFO] first line of the block"), std::string::npos) << lines[0];
        EXPECT_NE(lines[1].find("[WARN] second line of the block"), std::string::npos) << lines[1];
    }

    TEST(LogBlock, LinesComeOutAsWithoutABlock)
    {
        UnreadStderrPipe pipe;
        ct_log(CTLevel::Info, "same {} line\n", 1);
        ct_log(CTLevel::Error, "same {} line\n", 2);
        const std::string alone = pipe.take();
        {
            CtLogBlock block;
            ct_log(CTLevel::Info, "same {} line\n", 1);
            ct_log(CTLevel::Error, "same {} line\n", 2);
        }
        EXPECT_EQ(pipe.take(), alone);
        EXPECT_FALSE(alone.empty());
    }

    TEST(LogBlock, NestedBlocksAreWrittenWhenTheOutermostEnds)
    {
        UnreadStderrPipe pipe;
        {
            CtLogBlock outer;
            ct_log(CTLevel::Info, "outer\n");
            {
                CtLogBlock inner;
                ct_log(CTLevel::Info, "inner\n");
            }
            EXPECT_EQ(pipe.take(), "");
        }
        const std::vector<std::string> lines = splitLines(pipe.take());
        ASSERT_EQ(lines.size(), 2u);
        EXPECT_NE(lines[0].find("outer"), std::string::npos);
        EXPECT_NE(lines[1].find("inner"), std::string::npos);
    }

    TEST(LogBlock, AnAbandonedBlockIsNotWritten)
    {
        UnreadStderrPipe pipe;
        {
            CtLogBlock block;
            ct_log(CTLevel::Info, "dropped\n");
            ct_log_block_abandon();
            ct_log(CTLevel::Error, "reported\n");
            EXPECT_NE(pipe.take().find("reported"), std::string::npos);
        }
        EXPECT_EQ(pipe.take(), "");
    }

#if !defined(_WIN32)
    // One block, many times larger than the pipe: write() takes it in pieces.
    TEST(LogBlock, ABlockLargerThanThePipeComesOutWhole)
    {
        constexpr int kLines = 4000;
        StderrPipe pipe(true, std::chrono::milliseconds(200));
        {
            CtLogBlock block;
            for (int i = 0; i < kLines; ++i)
            {
                ct_log(CTLevel::Info, "line {} of one large block, padded to fill the pipe\n", i);
            }
        }
        const std::vector<std::string> lines = splitLines(pipe.finish());
        ASSERT_EQ(lines.size(), static_cast<size_t>(kLines));
        for (int i = 0; i < kLines; ++i)
        {
            const std::string expected = "line " + std::to_string(i) + " of one large block";
            ASSERT_NE(lines[i].find(expected), std::string::npos) << lines[i];
        }
    }
#endif

    // Allocation boxes logged by several threads at once: each box comes out whole. The
    // values of a box name its thread, so a line of another thread inside it shows.
    TEST(LogOutput, BoxesOfConcurrentThreadsDoNotInterleave)
    {
        constexpr int kThreads = 8;
        constexpr int kBoxes = 200;
        constexpr int kLinesPerBox = 7;
        StderrPipe pipe;
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t)
        {
            threads.emplace_back(
                [t, &go]
                {
                    while (!go.load())
                    {
                    }
                    const std::string label = "box-thread-" + std::to_string(t);
                    const std::string status = "thread-" + std::to_string(t);
                    for (int i = 0; i < kBoxes; ++i)
                    {
                        ct_log_alloc_details(label.c_str(), status.c_str(), 1000 + t, 2000 + t,
                                             reinterpret_cast<void*>(uintptr_t{0x1000} + t),
                                             nullptr, CTColor::Green, CTLevel::Info);
                    }
                });
        }
        go.store(true);
        for (std::thread& thread : threads)
        {
            thread.join();
        }
        const std::vector<std::string> lines = splitLines(pipe.finish());

        ASSERT_EQ(lines.size(), static_cast<size_t>(kThreads * kBoxes * kLinesPerBox));
        for (size_t box = 0; box < lines.size(); box += kLinesPerBox)
        {
            const std::string& header = lines[box];
            const size_t at = header.find("box-thread-");
            ASSERT_NE(at, std::string::npos) << "line " << box << ": " << header;
            const std::string thread = header.substr(at + 11, 1);
            EXPECT_NE(lines[box + 2].find("thread-" + thread), std::string::npos)
                << "line " << box + 2 << ": " << lines[box + 2];
            EXPECT_NE(lines[box + 3].find("100" + thread), std::string::npos)
                << "line " << box + 3 << ": " << lines[box + 3];
            EXPECT_NE(lines[box + 4].find("200" + thread), std::string::npos)
                << "line " << box + 4 << ": " << lines[box + 4];
            EXPECT_NE(lines[box + 5].find("0x100" + thread), std::string::npos)
                << "line " << box + 5 << ": " << lines[box + 5];
            if (testing::Test::HasFailure())
            {
                break;
            }
        }
    }
} // namespace
