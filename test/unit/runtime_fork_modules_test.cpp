// SPDX-License-Identifier: Apache-2.0
//
// glibc does not reset in the child of fork() the lock that dl_iterate_phdr takes: when another
// thread of the parent, of the runtime or not, was in dl_iterate_phdr at the fork, the child
// waits for it forever on its first call (#206). The vtable boxes of a child must therefore
// name modules without dl_iterate_phdr, and the runtime must not hold that lock in the parent
// either, where it would leave the child's own dlopen waiting. This program replaces
// dl_iterate_phdr for the runtime it links: in a child of the test, it never returns, as if that
// lock had been inherited held, and in the test, it counts the calls. Linux only. The libraries
// are built from reload/ct_reload_library.cpp.
#include "ct_runtime_internal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>

#include <dlfcn.h>
#include <link.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C"
{
    void __ct_vcall_trace(void* this_ptr, void* target, const char* site, const char* static_type);
}

namespace
{
    using Callback = int (*)(dl_phdr_info*, size_t, void*);
    using IteratePhdr = int (*)(Callback, void*);

    // The process of the test; 0 before it starts, when every call goes through.
    std::atomic<pid_t> test_process{0};
    // The calls in the process of the test.
    std::atomic<int> listings{0};
} // namespace

extern "C" int dl_iterate_phdr(Callback callback, void* data)
{
    const pid_t test = test_process.load();
    if (test != 0 && getpid() != test)
    {
        for (;;)
        {
            pause();
        }
    }
    if (test != 0)
    {
        listings.fetch_add(1);
    }
    auto* real = reinterpret_cast<IteratePhdr>(dlsym(RTLD_NEXT, "dl_iterate_phdr"));
    return real(callback, data);
}

namespace
{
    struct Loaded
    {
        void* handle = nullptr;
        void* object = nullptr;
        void* target = nullptr;
    };

    Loaded load(const char* path)
    {
        Loaded library;
        library.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (library.handle)
        {
            using Entry = void* (*)();
            library.object = reinterpret_cast<Entry>(dlsym(library.handle, "ct_reload_object"))();
            library.target = reinterpret_cast<Entry>(dlsym(library.handle, "ct_reload_target"))();
        }
        return library;
    }

    std::string traceACall(const Loaded& library)
    {
        testing::internal::CaptureStderr();
        __ct_vcall_trace(library.object, library.target, "fork:1", "<unknown>");
        return testing::internal::GetCapturedStderr();
    }

    // Whether the box names the module of the target.
    bool namesTheTarget(const std::string& box, const char* library)
    {
        return box.find("tmod") != std::string::npos && box.find(library) != std::string::npos;
    }

    // The line of the box that has the label.
    std::string lineOf(const std::string& box, const char* label)
    {
        const size_t at = box.find(label);
        if (at == std::string::npos)
        {
            return std::string();
        }
        const size_t start = box.rfind('\n', at);
        const size_t end = box.find('\n', at);
        return box.substr(start == std::string::npos ? 0 : start + 1,
                          end == std::string::npos ? std::string::npos : end - start - 1);
    }

    // A type and a function of the main program.
    struct Local
    {
        virtual ~Local() = default;
        virtual int value() const
        {
            return 3;
        }
    };

    Local local;

    int describeLocal(int value)
    {
        return local.value() + value;
    }

    // A child of the test in a process group of its own, with the children it forks.
    pid_t forkGroup()
    {
        const pid_t child = fork();
        if (child == 0)
        {
            setpgid(0, 0);
        }
        else if (child > 0)
        {
            setpgid(child, child);
        }
        return child;
    }

    // Whether the child exited with status 0 within two seconds; it is killed otherwise, with
    // its group if it leads one: a grandchild that never returns must not outlive the test.
    bool exitsInTime(pid_t child)
    {
        int status = 0;
        for (int waited = 0; waited < 2000; ++waited)
        {
            if (waitpid(child, &status, WNOHANG) == child)
            {
                return WIFEXITED(status) && WEXITSTATUS(status) == 0;
            }
            usleep(1000);
        }
        kill(-child, SIGKILL);
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        return false;
    }

    class ForkModules : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            saved_ = ct_get_features();
            ct_set_enabled(CT_FEATURE_VTABLE_DIAG, 1);
            test_process.store(getpid());
        }
        void TearDown() override
        {
            test_process.store(0);
            ct_set_enabled(CT_FEATURE_VTABLE_DIAG, (saved_ & CT_FEATURE_VTABLE_DIAG) != 0);
        }

      private:
        unsigned saved_ = 0;
    };

    // A thread of the parent in dl_iterate_phdr at the fork leaves the child's own dlopen waiting
    // forever: the runtime must not be that thread.
    TEST_F(ForkModules, AParentNamesAModuleWithoutListingTheModules)
    {
        const Loaded library = load(CT_RELOAD_LIBRARY_A);
        ASSERT_NE(library.handle, nullptr) << dlerror();
        const int before = listings.load();
        EXPECT_TRUE(namesTheTarget(traceACall(library), "ct_reload_a"));
        EXPECT_EQ(listings.load(), before);
        dlclose(library.handle);
    }

    // The child names the main program "main", as its parent does, whatever argv[0] has become,
    // as setproctitle leaves it: dladdr names the main program by argv[0].
    TEST_F(ForkModules, AChildNamesTheMainProgramMain)
    {
        const Loaded program{nullptr, &local, reinterpret_cast<void*>(&describeLocal)};
        ASSERT_NE(lineOf(traceACall(program), "tmod").find("main"), std::string::npos);

        const pid_t child = forkGroup();
        if (child == 0)
        {
            std::memset(program_invocation_name, 'X', std::strlen(program_invocation_name));
            _exit(lineOf(traceACall(program), "tmod").find("main") != std::string::npos ? 0 : 1);
        }
        ASSERT_GT(child, 0);
        EXPECT_TRUE(exitsInTime(child)) << "the child did not name the main program main";
    }

    // The child names a module its parent loaded and named.
    TEST_F(ForkModules, AChildNamesAModuleItsParentLoaded)
    {
        const Loaded library = load(CT_RELOAD_LIBRARY_A);
        ASSERT_NE(library.handle, nullptr) << dlerror();
        ASSERT_TRUE(namesTheTarget(traceACall(library), "ct_reload_a"));

        const pid_t child = forkGroup();
        if (child == 0)
        {
            _exit(namesTheTarget(traceACall(library), "ct_reload_a") ? 0 : 1);
        }
        ASSERT_GT(child, 0);
        EXPECT_TRUE(exitsInTime(child)) << "the child did not name the module within 2 seconds";
        dlclose(library.handle);
    }

    // The child names a module it loaded, where its parent may have unloaded another one after
    // naming it: nothing the parent listed before the fork names the child's modules.
    TEST_F(ForkModules, AChildNamesAModuleItLoaded)
    {
        const Loaded unloaded = load(CT_RELOAD_LIBRARY_A);
        ASSERT_NE(unloaded.handle, nullptr) << dlerror();
        ASSERT_TRUE(namesTheTarget(traceACall(unloaded), "ct_reload_a"));
        dlclose(unloaded.handle);

        const pid_t child = forkGroup();
        if (child == 0)
        {
            const Loaded library = load(CT_RELOAD_LIBRARY_B);
            const std::string box = library.handle ? traceACall(library) : std::string();
            _exit(namesTheTarget(box, "ct_reload_b") && box.find("ct_reload_a") == std::string::npos
                      ? 0
                      : 1);
        }
        ASSERT_GT(child, 0);
        EXPECT_TRUE(exitsInTime(child)) << "the child did not name the module within 2 seconds";
    }

    // A child of a child names the modules its grandparent listed.
    TEST_F(ForkModules, AGrandchildNamesTheModulesToo)
    {
        const Loaded library = load(CT_RELOAD_LIBRARY_A);
        ASSERT_NE(library.handle, nullptr) << dlerror();
        ASSERT_TRUE(namesTheTarget(traceACall(library), "ct_reload_a"));

        const pid_t child = forkGroup();
        if (child == 0)
        {
            const pid_t grandchild = fork();
            if (grandchild == 0)
            {
                _exit(namesTheTarget(traceACall(library), "ct_reload_a") ? 0 : 1);
            }
            _exit(grandchild > 0 && exitsInTime(grandchild) ? 0 : 1);
        }
        ASSERT_GT(child, 0);
        EXPECT_TRUE(exitsInTime(child)) << "the grandchild did not name the module in time";
        dlclose(library.handle);
    }
} // namespace
