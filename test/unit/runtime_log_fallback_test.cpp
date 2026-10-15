// SPDX-License-Identifier: Apache-2.0
//
// The fallback of the runtime's sink: when a CtLogBlock cannot grow, what it holds and the
// line that did not fit go to stderr at once, under the log lock, while other threads write
// their blocks. Nothing may block, and no line may interleave with another.
//
// An executable of its own: it replaces operator new, which fails, once each time the thread
// arms it, the first request of at least kFailFrom bytes. The lines are shorter than that,
// so only the growth of a block's buffer fails. POSIX only.
#include "ct_runtime_internal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <map>
#include <new>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

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
}

namespace
{
    constexpr size_t kFailFrom = 4096;
    thread_local bool t_armed = false;
    std::atomic<int> g_injected_failures{0};
} // namespace

void* operator new(std::size_t size)
{
    if (t_armed && size >= kFailFrom)
    {
        t_armed = false;
        g_injected_failures.fetch_add(1);
        throw std::bad_alloc();
    }
    if (void* block = std::malloc(size != 0 ? size : 1))
    {
        return block;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

// The nothrow forms call the forms above, as the standard's do. They must be replaced too:
// AddressSanitizer brings its own, whose blocks this file's operator delete could not free.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return ::operator new(size);
    }
    catch (const std::bad_alloc&)
    {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept
{
    return ::operator new(size, tag);
}

void operator delete(void* block) noexcept
{
    std::free(block);
}

void operator delete[](void* block) noexcept
{
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept
{
    std::free(block);
}

void operator delete[](void* block, std::size_t) noexcept
{
    std::free(block);
}

void operator delete(void* block, const std::nothrow_t&) noexcept
{
    std::free(block);
}

void operator delete[](void* block, const std::nothrow_t&) noexcept
{
    std::free(block);
}

namespace
{
    using namespace std::chrono_literals;

    constexpr int kWriters = 4;
    constexpr int kBlocks = 40;
    constexpr int kLinesPerBlock = 60;
    // The writer whose blocks fail to grow, each one after kArmedAfter lines.
    constexpr int kArmedWriter = 0;
    constexpr int kArmedAfter = 30;
    const std::string kPadding(120, 'x');

    // Stderr on a pipe for the life of the object; a thread reads it slowly, so that the pipe
    // fills and the kernel takes large writes in pieces.
    class SlowlyReadStderr
    {
      public:
        SlowlyReadStderr()
        {
            int fds[2];
            EXPECT_EQ(pipe(fds), 0);
            read_end_ = fds[0];
            saved_ = dup(2);
            EXPECT_EQ(dup2(fds[1], 2), 2);
            close(fds[1]);
            reader_ = std::thread(
                [this]
                {
                    char buffer[1024];
                    ssize_t got = 0;
                    while ((got = read(read_end_, buffer, sizeof(buffer))) > 0)
                    {
                        text_.append(buffer, static_cast<size_t>(got));
                        std::this_thread::sleep_for(20us);
                    }
                });
        }

        // Restores stderr and returns all that was written to the pipe.
        std::string finish()
        {
            dup2(saved_, 2);
            close(saved_);
            reader_.join();
            close(read_end_);
            return text_;
        }

      private:
        int read_end_ = -1;
        int saved_ = -1;
        std::thread reader_;
        std::string text_;
    };

    void writeBlocks(int writer)
    {
        for (int block = 0; block < kBlocks; ++block)
        {
            CtLogBlock box;
            for (int line = 0; line < kLinesPerBlock; ++line)
            {
                if (writer == kArmedWriter && line == kArmedAfter)
                {
                    t_armed = true;
                }
                ct_log(CTLevel::Info, "W{} B{} L{} {}\n", writer, block, line, kPadding);
            }
            t_armed = false;
        }
    }

    struct Line
    {
        int writer;
        int block;
        int line;
    };

    TEST(LogFallback, BlocksThatCannotGrowAreWrittenWholeWhileOthersWrite)
    {
        SlowlyReadStderr stderr_pipe;
        std::vector<std::thread> writers;
        for (int writer = 0; writer < kWriters; ++writer)
        {
            writers.emplace_back(writeBlocks, writer);
        }
        // No writer may block: they all end well within the time the test allows.
        std::future<void> all_done = std::async(std::launch::async,
                                                [&]
                                                {
                                                    for (std::thread& writer : writers)
                                                    {
                                                        writer.join();
                                                    }
                                                });
        ASSERT_EQ(all_done.wait_for(60s), std::future_status::ready) << "a writer is blocked";
        const std::string output = stderr_pipe.finish();

        EXPECT_EQ(g_injected_failures.load(), kBlocks) << "the fallback did not run each time";

        // Every line whole, in the order its writer wrote it; every block in one run of lines,
        // but those whose buffer could not grow, in two at most: the fallback, then the rest.
        const std::regex shape("^\\|[0-9]+\\| ==ct== \\[INFO\\] W([0-9]+) B([0-9]+) L([0-9]+) " +
                               kPadding + "$");
        std::vector<Line> lines;
        std::istringstream stream(output);
        std::string text;
        size_t broken = 0;
        while (std::getline(stream, text))
        {
            std::smatch match;
            if (!std::regex_match(text, match, shape))
            {
                if (++broken <= 3)
                {
                    ADD_FAILURE() << "broken line: " << text.substr(0, 200);
                }
                continue;
            }
            lines.push_back({std::stoi(match[1]), std::stoi(match[2]), std::stoi(match[3])});
        }
        EXPECT_EQ(broken, 0u);
        EXPECT_EQ(lines.size(), static_cast<size_t>(kWriters * kBlocks * kLinesPerBlock));

        std::map<std::pair<int, int>, int> runs;
        std::map<int, int> next_line_of_writer;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const Line& line = lines[i];
            const int expected = next_line_of_writer[line.writer]++;
            EXPECT_EQ(line.block * kLinesPerBlock + line.line, expected)
                << "writer " << line.writer << " out of order";
            if (i == 0 || lines[i - 1].writer != line.writer || lines[i - 1].block != line.block)
            {
                ++runs[{line.writer, line.block}];
            }
        }
        for (const auto& [block, count] : runs)
        {
            const bool armed = block.first == kArmedWriter;
            EXPECT_LE(count, armed ? 2 : 1) << "block " << block.second << " of writer "
                                            << block.first << " in " << count << " pieces";
        }
    }
} // namespace
