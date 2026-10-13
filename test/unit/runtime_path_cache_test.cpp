// SPDX-License-Identifier: Apache-2.0
//
// The cache of resolved module paths behind the vtable diagnostics (#185): it answers as
// realpath() does, resolves a path once while the path leads to the same file, and resolves it
// again when a symbolic link on it changes target or the file is replaced, as when a library is
// unloaded and another loaded through the same path. POSIX only, as the runtime that uses it.
#include "ct_runtime_path_cache.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>
#if defined(__linux__)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
    namespace fs = std::filesystem;

    std::atomic<int> resolved_count{0};

    // realpath(), counted.
    bool countingResolver(const char* path, std::string& out)
    {
        resolved_count.fetch_add(1);
        return ct_realpath(path, out);
    }

    std::string realpathOf(const fs::path& path)
    {
        char resolved[PATH_MAX];
        return realpath(path.c_str(), resolved) ? std::string(resolved) : std::string();
    }

    // A directory of its own for each test, removed at the end.
    class Scratch
    {
      public:
        Scratch()
        {
            std::string pattern = (fs::temp_directory_path() / "ct_path_cache_XXXXXX").string();
            const char* made = mkdtemp(pattern.data());
            EXPECT_NE(made, nullptr);
            root_ = made ? fs::path(made) : fs::path();
        }
        ~Scratch()
        {
            std::error_code ignored;
            fs::remove_all(root_, ignored);
        }
        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;

        fs::path file(const std::string& name, const std::string& text = "x") const
        {
            const fs::path path = root_ / name;
            std::ofstream(path) << text;
            return path;
        }
        // A symbolic link at `name` to `target`, replacing the one there.
        fs::path link(const std::string& name, const fs::path& target) const
        {
            const fs::path path = root_ / name;
            std::error_code ignored;
            fs::remove(path, ignored);
            fs::create_symlink(target, path);
            return path;
        }
        const fs::path& root() const
        {
            return root_;
        }

      private:
        fs::path root_;
    };

    TEST(PathCache, ResolvesAsRealpath)
    {
        Scratch scratch;
        const fs::path file = scratch.file("module.so");
        fs::create_directory(scratch.root() / "real");
        const fs::path nested = scratch.file("real/inner.so");
        const fs::path direct = scratch.link("direct.so", file);
        const fs::path through_dir = scratch.link("dir", scratch.root() / "real") / "inner.so";
        CtPathCache cache(countingResolver);
        for (const fs::path& path : {file, nested, direct, through_dir})
        {
            std::string out;
            ASSERT_TRUE(cache.resolve(path.c_str(), out)) << path;
            EXPECT_EQ(out, realpathOf(path)) << path;
        }
    }

    TEST(PathCache, ResolvesAPathOnceWhileItLeadsToTheSameFile)
    {
        Scratch scratch;
        const fs::path link = scratch.link("module.so", scratch.file("v1.so"));
        CtPathCache cache(countingResolver);
        resolved_count = 0;
        std::string out;
        for (int i = 0; i < 10; ++i)
        {
            ASSERT_TRUE(cache.resolve(link.c_str(), out));
            EXPECT_EQ(out, realpathOf(scratch.root() / "v1.so"));
        }
        EXPECT_EQ(resolved_count.load(), 1);
        EXPECT_EQ(cache.size(), 1u);
    }

    TEST(PathCache, FollowsASymbolicLinkThatChangesTarget)
    {
        Scratch scratch;
        const fs::path v1 = scratch.file("v1.so");
        const fs::path v2 = scratch.file("v2.so");
        const fs::path link = scratch.link("module.so", v1);
        CtPathCache cache(countingResolver);
        std::string out;
        ASSERT_TRUE(cache.resolve(link.c_str(), out));
        EXPECT_EQ(out, realpathOf(v1));

        scratch.link("module.so", v2);
        ASSERT_TRUE(cache.resolve(link.c_str(), out));
        EXPECT_EQ(out, realpathOf(v2));
        scratch.link("module.so", v1);
        ASSERT_TRUE(cache.resolve(link.c_str(), out));
        EXPECT_EQ(out, realpathOf(v1));
    }

    TEST(PathCache, FollowsALinkedDirectoryThatChangesTarget)
    {
        Scratch scratch;
        fs::create_directory(scratch.root() / "release-1");
        fs::create_directory(scratch.root() / "release-2");
        scratch.file("release-1/module.so");
        scratch.file("release-2/module.so");
        const fs::path path = scratch.link("current", scratch.root() / "release-1") / "module.so";
        CtPathCache cache(countingResolver);
        std::string out;
        ASSERT_TRUE(cache.resolve(path.c_str(), out));
        EXPECT_EQ(out, realpathOf(scratch.root() / "release-1" / "module.so"));

        scratch.link("current", scratch.root() / "release-2");
        ASSERT_TRUE(cache.resolve(path.c_str(), out));
        EXPECT_EQ(out, realpathOf(scratch.root() / "release-2" / "module.so"));
    }

    // A library rebuilt in place: a new file at the same path.
    TEST(PathCache, ResolvesAgainAFileReplacedAtItsPath)
    {
        Scratch scratch;
        const fs::path file = scratch.file("module.so", "first");
        CtPathCache cache(countingResolver);
        resolved_count = 0;
        std::string out;
        ASSERT_TRUE(cache.resolve(file.c_str(), out));
        fs::rename(scratch.file("module.so.new", "second"), file);
        ASSERT_TRUE(cache.resolve(file.c_str(), out));
        EXPECT_EQ(out, realpathOf(file));
        EXPECT_EQ(resolved_count.load(), 2);
    }

    TEST(PathCache, FailsAsRealpathForAMissingPathAndResolvesItOnceCreated)
    {
        Scratch scratch;
        const fs::path path = scratch.root() / "later.so";
        CtPathCache cache(countingResolver);
        std::string out = "unchanged";
        EXPECT_FALSE(cache.resolve(path.c_str(), out));
        EXPECT_EQ(out, "unchanged");
        EXPECT_FALSE(cache.resolve(nullptr, out));
        EXPECT_FALSE(cache.resolve("", out));

        scratch.file("later.so");
        ASSERT_TRUE(cache.resolve(path.c_str(), out));
        EXPECT_EQ(out, realpathOf(path));

        // A dangling link fails as realpath() does.
        const fs::path dangling = scratch.link("dangling.so", scratch.root() / "gone.so");
        EXPECT_FALSE(cache.resolve(dangling.c_str(), out));
    }

    // Two paths to one file may resolve to different texts, which its identity cannot tell
    // apart: such a file is resolved every time.
    TEST(PathCache, ResolvesAFileWithSeveralLinksEveryTime)
    {
        Scratch scratch;
        const fs::path file = scratch.file("module.so");
        const fs::path other = scratch.root() / "alias.so";
        fs::create_hard_link(file, other);
        CtPathCache cache(countingResolver);
        resolved_count = 0;
        std::string out;
        for (int i = 0; i < 3; ++i)
        {
            ASSERT_TRUE(cache.resolve(other.c_str(), out));
            EXPECT_EQ(out, realpathOf(other));
        }
        EXPECT_EQ(resolved_count.load(), 3);
        EXPECT_EQ(cache.size(), 0u);
    }

    TEST(PathCache, KeepsAnsweringPastItsCapacity)
    {
        Scratch scratch;
        CtPathCache cache(countingResolver);
        const size_t count = CtPathCache::kMaxEntries + 20;
        std::vector<fs::path> paths;
        for (size_t i = 0; i < count; ++i)
        {
            paths.push_back(scratch.file("module" + std::to_string(i) + ".so"));
            std::string out;
            ASSERT_TRUE(cache.resolve(paths.back().c_str(), out)) << i;
            ASSERT_EQ(out, realpathOf(paths.back()));
        }
        EXPECT_EQ(cache.size(), CtPathCache::kMaxEntries);

        resolved_count = 0;
        std::string out;
        ASSERT_TRUE(cache.resolve(paths.front().c_str(), out));
        EXPECT_EQ(resolved_count.load(), 0);
        ASSERT_TRUE(cache.resolve(paths.back().c_str(), out));
        EXPECT_EQ(out, realpathOf(paths.back()));
        EXPECT_EQ(resolved_count.load(), 1);
    }

    TEST(PathCache, ThreadsGetThePathsTheyAskFor)
    {
        Scratch scratch;
        std::vector<fs::path> paths;
        for (int i = 0; i < 20; ++i)
        {
            paths.push_back(scratch.link("link" + std::to_string(i) + ".so",
                                         scratch.file("module" + std::to_string(i) + ".so")));
        }
        CtPathCache cache(countingResolver);
        std::atomic<int> wrong{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
        {
            threads.emplace_back(
                [&, t]
                {
                    std::string out;
                    for (int i = 0; i < 2000; ++i)
                    {
                        const fs::path& path = paths[(i + t) % paths.size()];
                        if (!cache.resolve(path.c_str(), out) || out != realpathOf(path))
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
        EXPECT_EQ(cache.size(), paths.size());
    }

    // A library loaded through a symbolic link, unloaded, the link pointed at another build,
    // loaded again through it: the path resolves to the library loaded now.
    TEST(PathCache, FollowsALibraryReloadedThroughALinkToAnotherBuild)
    {
        Scratch scratch;
        CtPathCache cache(countingResolver);
        const struct
        {
            const char* library;
            const char* name;
        } builds[] = {{CT_RELOAD_LIBRARY_A, "reload_a"},
                      {CT_RELOAD_LIBRARY_B, "reload_b"},
                      {CT_RELOAD_LIBRARY_A, "reload_a"}};
        for (const auto& build : builds)
        {
            SCOPED_TRACE(build.name);
            const fs::path link = scratch.link("module.so", build.library);
            void* handle = dlopen(link.c_str(), RTLD_NOW | RTLD_LOCAL);
            ASSERT_NE(handle, nullptr) << dlerror();
            // The library loaded is that build: its function is in its namespace.
            auto* target = reinterpret_cast<void* (*)()>(dlsym(handle, "ct_reload_target"));
            ASSERT_NE(target, nullptr);
            Dl_info info;
            ASSERT_NE(dladdr(target(), &info), 0);
            ASSERT_NE(info.dli_sname, nullptr);
            EXPECT_NE(std::string(info.dli_sname).find(build.name), std::string::npos)
                << info.dli_sname;

            std::string out;
            ASSERT_TRUE(cache.resolve(link.c_str(), out));
            EXPECT_EQ(out, realpathOf(build.library));
            dlclose(handle);
        }
    }

#if defined(__linux__)
    // fork() copies only the calling thread: had it not waited for the thread holding the
    // cache's lock, the lock would stay held in the child, which would hang on its first path.
    // Not on macOS, for the reason runtime_demangle_cache_test.cpp gives (#207).
    TEST(PathCache, AChildForkedWhileAnotherThreadHoldsTheLockCanUseIt)
    {
        Scratch scratch;
        const fs::path file = scratch.file("module.so");
        CtPathCache cache(countingResolver);
        std::atomic<bool> held{false};
        std::thread holder(
            [&held]
            {
                ct_path_cache_lock_acquire();
                held.store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                ct_path_cache_lock_release();
            });
        while (!held.load())
        {
        }
        const pid_t child = fork();
        if (child == 0)
        {
            std::string out;
            _exit(cache.resolve(file.c_str(), out) && out == realpathOf(file) ? 0 : 1);
        }
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
} // namespace
