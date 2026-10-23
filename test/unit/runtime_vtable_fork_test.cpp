// SPDX-License-Identifier: Apache-2.0
//
// With the vtable diagnostics, a virtual call box names the modules of the vtable and of the
// target. fork() copies only the calling thread, and glibc does not reset in the child the
// lock that dl_iterate_phdr takes: a child that looked modules up through it waited forever
// when another thread of the parent was in dl_iterate_phdr at the fork (#206). Here a thread
// stays in dl_iterate_phdr until the parent has forked. Linux only. The library is built from
// reload/ct_reload_library.cpp.
#include "ct_runtime_internal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include <dlfcn.h>
#include <fcntl.h>
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
    std::atomic<bool> listing{false};
    std::atomic<bool> forked{false};

    // Keeps dl_iterate_phdr, and the loader's lock it holds, until the parent has forked.
    int holdTheModuleList(dl_phdr_info*, size_t, void*)
    {
        listing.store(true);
        while (!forked.load())
        {
            usleep(1000);
        }
        return 1;
    }

    // Whether the child exited with status 0 within two seconds; it is killed otherwise.
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
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        return false;
    }

    TEST(VtableFork, AChildNamesModulesThatAnotherThreadWasListing)
    {
        void* library = dlopen(CT_RELOAD_LIBRARY_A, RTLD_NOW | RTLD_LOCAL);
        ASSERT_NE(library, nullptr) << dlerror();
        using Entry = void* (*)();
        void* object = reinterpret_cast<Entry>(dlsym(library, "ct_reload_object"))();
        void* target = reinterpret_cast<Entry>(dlsym(library, "ct_reload_target"))();
        const unsigned saved = ct_get_features();
        ct_set_enabled(CT_FEATURE_VTABLE_DIAG, 1);

        listing.store(false);
        forked.store(false);
        std::thread lister([] { dl_iterate_phdr(holdTheModuleList, nullptr); });
        while (!listing.load())
        {
            usleep(1000);
        }
        const pid_t child = fork();
        if (child == 0)
        {
            const int null = open("/dev/null", O_WRONLY);
            dup2(null, STDERR_FILENO);
            __ct_vcall_trace(object, target, "fork:1", "<unknown>");
            _exit(0);
        }
        forked.store(true);
        lister.join();

        ct_set_enabled(CT_FEATURE_VTABLE_DIAG, (saved & CT_FEATURE_VTABLE_DIAG) != 0);
        dlclose(library);
        ASSERT_GT(child, 0);
        EXPECT_TRUE(exitsInTime(child)) << "the child did not exit within two seconds";
    }
} // namespace
