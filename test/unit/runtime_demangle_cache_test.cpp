// SPDX-License-Identifier: Apache-2.0
//
// The cache of demangled names behind the vtable boxes: it answers as its demangler does,
// decodes a name once, holds a bounded number of names, and looks a name up by its text, so
// that a name at the address of another, as when a library is unloaded and another loaded
// in its place, gets its own result. A name its demangler could not decode for now, as when
// DbgHelp was busy, is decoded again next time.
#include "ct_runtime_demangle_cache.h"
#include "ct_runtime_helpers.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
    std::atomic<int> decoded{0};

    // Decodes "m:<text>" to "<text>", and nothing else; counts its calls.
    bool countingDemangler(const char* name, std::string& out)
    {
        decoded.fetch_add(1);
        if (!name || std::strncmp(name, "m:", 2) != 0)
        {
            return false;
        }
        out.assign(name + 2);
        return true;
    }

    std::atomic<int> attempts{0};
    std::atomic<bool> available{false};

    // Decodes a name to itself once `available`, and nothing until then; counts its calls.
    CtDemangleOutcome waitingDemangler(const char* name, std::string& out)
    {
        attempts.fetch_add(1);
        if (!available.load())
        {
            return CtDemangleOutcome::Unavailable;
        }
        out.assign(name);
        return CtDemangleOutcome::Decoded;
    }

    // Contention, release, then another try: the name is decoded once its demangler can.
    TEST(DemangleCache, DecodesAgainANameItsDemanglerCouldNotDecodeForNow)
    {
        attempts.store(0);
        available.store(false);
        CtDemangleCache cache(waitingDemangler);
        std::string out;
        EXPECT_FALSE(cache.demangle("m:later", out));
        EXPECT_EQ(cache.size(), 0u);

        available.store(true);
        ASSERT_TRUE(cache.demangle("m:later", out));
        EXPECT_EQ(out, "m:later");
        ASSERT_TRUE(cache.demangle("m:later", out));
        EXPECT_EQ(attempts.load(), 2);
        EXPECT_EQ(cache.size(), 1u);
    }

    TEST(DemangleCache, AnswersAsItsDemangler)
    {
        CtDemangleCache cache(countingDemangler);
        std::string out;
        ASSERT_TRUE(cache.demangle("m:first", out));
        EXPECT_EQ(out, "first");
        out = "unchanged";
        EXPECT_FALSE(cache.demangle("plain", out));
        EXPECT_EQ(out, "unchanged");
        EXPECT_FALSE(cache.demangle(nullptr, out));
    }

    TEST(DemangleCache, DecodesANameOnce)
    {
        CtDemangleCache cache(countingDemangler);
        decoded = 0;
        std::string out;
        for (int i = 0; i < 10; ++i)
        {
            ASSERT_TRUE(cache.demangle("m:again", out));
            EXPECT_EQ(out, "again");
            EXPECT_FALSE(cache.demangle("undecorated", out));
        }
        EXPECT_EQ(decoded.load(), 2);
    }

    // One buffer holds a name, then another: the second is not answered with the first's
    // result, as a stale entry keyed by address would.
    TEST(DemangleCache, ANameAtTheAddressOfAnotherGetsItsOwnResult)
    {
        CtDemangleCache cache(countingDemangler);
        char name[32];
        std::string out;
        std::strcpy(name, "m:unloaded");
        ASSERT_TRUE(cache.demangle(name, out));
        EXPECT_EQ(out, "unloaded");

        std::strcpy(name, "m:loaded");
        ASSERT_TRUE(cache.demangle(name, out));
        EXPECT_EQ(out, "loaded");
        std::strcpy(name, "not a name");
        EXPECT_FALSE(cache.demangle(name, out));
    }

    TEST(DemangleCache, KeepsAnsweringPastItsCapacity)
    {
        CtDemangleCache cache(countingDemangler);
        const size_t count = CtDemangleCache::kMaxEntries + 100;
        std::string out;
        for (size_t i = 0; i < count; ++i)
        {
            const std::string name = "m:name" + std::to_string(i);
            ASSERT_TRUE(cache.demangle(name.c_str(), out)) << i;
            ASSERT_EQ(out, "name" + std::to_string(i));
        }
        EXPECT_EQ(cache.size(), CtDemangleCache::kMaxEntries);

        // The names it holds are answered from it; the others are decoded each time.
        decoded = 0;
        ASSERT_TRUE(cache.demangle("m:name0", out));
        EXPECT_EQ(decoded.load(), 0);
        const std::string last = "m:name" + std::to_string(count - 1);
        ASSERT_TRUE(cache.demangle(last.c_str(), out));
        EXPECT_EQ(out, "name" + std::to_string(count - 1));
        EXPECT_EQ(decoded.load(), 1);
    }

    TEST(DemangleCache, ThreadsGetTheNamesTheyAskFor)
    {
        CtDemangleCache cache(countingDemangler);
        constexpr int kThreads = 8;
        constexpr int kRounds = 2000;
        std::atomic<int> wrong{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t)
        {
            threads.emplace_back(
                [t, &cache, &wrong]
                {
                    std::string out;
                    for (int i = 0; i < kRounds; ++i)
                    {
                        const std::string text = "shared" + std::to_string((i + t) % 50);
                        const std::string name = "m:" + text;
                        if (!cache.demangle(name.c_str(), out) || out != text)
                        {
                            wrong.fetch_add(1);
                        }
                    }
                });
        }
        for (std::thread& thread : threads)
        {
            thread.join();
        }
        EXPECT_EQ(wrong.load(), 0);
        EXPECT_EQ(cache.size(), 50u);
    }

#if defined(__linux__)
    // fork() copies only the calling thread: had it not waited for the thread holding the
    // cache's lock, the lock would stay held in the child, which would hang on its first name.
    // Not on macOS: in this process, which tracks Objective-C objects, a child of fork() hangs
    // before it returns, whatever this cache does. libSystem's handler releases an XPC object
    // in the child, and the runtime's hook of -dealloc waits for the allocation table's lock,
    // which the runtime's own child handler, run after it, has yet to release.
    TEST(DemangleCache, AChildForkedWhileAnotherThreadHoldsTheLockCanUseIt)
    {
        CtDemangleCache cache(countingDemangler);
        std::atomic<bool> held{false};
        std::atomic<bool> forked{false};
        std::thread holder(
            [&held, &forked]
            {
                ct_demangle_cache_lock_acquire();
                held.store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                ct_demangle_cache_lock_release();
                // Alive until fork() has returned in the parent: a thread that had ended,
                // unjoined, by then would be copied into the child as such, and
                // ThreadSanitizer would report it as leaked there.
                while (!forked.load())
                {
                    std::this_thread::yield();
                }
            });
        while (!held.load())
        {
        }
        const pid_t child = fork();
        if (child == 0)
        {
            std::string out;
            _exit(cache.demangle("m:child", out) && out == "child" ? 0 : 1);
        }
        // Whether fork() succeeded or not, so that the holder ends and is joined.
        forked.store(true);
        holder.join();
        ASSERT_GT(child, 0);

        int status = 0;
        bool exited = false;
        for (int waited = 0; waited < 2000 && !exited; ++waited)
        {
            exited = waitpid(child, &status, WNOHANG) == child;
            if (!exited)
            {
                usleep(1000);
            }
        }
        if (!exited)
        {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
        }
        ASSERT_TRUE(exited) << "the child hung on the cache's lock";
        EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
#endif

    // With the platform's demangler, as the vtable boxes use it.
    TEST(DemangleCache, DecodesThePlatformCppNames)
    {
        CtDemangleCache cache(ct_demangle);
        std::string out;
#if defined(_WIN32)
        ASSERT_TRUE(cache.demangle("?compute@@YAHH@Z", out));
        EXPECT_NE(out.find("compute(int)"), std::string::npos) << out;
#else
        ASSERT_TRUE(cache.demangle("_Z7computei", out));
        EXPECT_EQ(out, "compute(int)");
#endif
        EXPECT_FALSE(cache.demangle("main", out));
    }
} // namespace
