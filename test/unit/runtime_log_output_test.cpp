// SPDX-License-Identifier: Apache-2.0
//
// What the runtime's log lines become on stderr: every byte written, in order, when the
// descriptor takes them in pieces. Each test points stderr at a pipe of its own and
// restores it.
#include "ct_runtime_internal.h"

#include <gtest/gtest.h>

#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
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
    int ct_test_dup2(int from, int to)
    {
        return _dup2(from, to);
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

} // namespace
