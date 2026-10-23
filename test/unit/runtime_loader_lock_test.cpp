// SPDX-License-Identifier: Apache-2.0
//
// dlopen holds the loader's dl_load_lock while it runs the constructors of the library it
// loads: a constructor that waits for a thread making traced virtual calls returns only if the
// runtime names the calls without that lock (#223). This program exports the runtime's entry
// points to the library, built from reload/ct_trace_constructor_library.cpp, and loads it in a
// child, which a hang leaves killed after ten seconds. Linux only.
#include "ct_runtime_internal.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C"
{
    void __ct_vcall_trace(void* this_ptr, void* target, const char* site, const char* static_type);
}

namespace
{
    // The exit status of the child within ten seconds, or -1: it is killed then.
    int exitStatusInTime(pid_t child)
    {
        int status = 0;
        for (int waited = 0; waited < 10000; ++waited)
        {
            if (waitpid(child, &status, WNOHANG) == child)
            {
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            usleep(1000);
        }
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        return -1;
    }

    TEST(LoaderLock, ALibraryConstructorThatWaitsForATracedCallReturns)
    {
#if defined(__GLIBC__) && !defined(DLFO_STRUCT_HAS_EH_DBASE)
        GTEST_SKIP() << "before glibc 2.35, the runtime names the target with dladdr, which "
                        "waits for the lock that dlopen holds";
#endif
        // Linked in, so that the library finds it among the exported symbols.
        void* volatile entry = reinterpret_cast<void*>(&__ct_vcall_trace);
        (void)entry;
        ct_set_enabled(CT_FEATURE_VTABLE_DIAG, 1);
        const std::string boxes =
            testing::TempDir() + "ct_loader_lock_" + std::to_string(getpid()) + ".txt";
        const pid_t child = fork();
        if (child == 0)
        {
            // The boxes go to a file, which the test reads once the child has exited.
            const int file = open(boxes.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (file < 0 || dup2(file, 2) != 2)
            {
                _exit(4);
            }
            _exit(dlopen(CT_TRACE_CONSTRUCTOR_LIBRARY, RTLD_NOW | RTLD_LOCAL) ? 0 : 3);
        }
        ASSERT_GT(child, 0);
        EXPECT_EQ(exitStatusInTime(child), 0)
            << "-1: dlopen did not return within ten seconds; 3: dlopen failed; 4: no file for "
               "the boxes";
        std::ifstream file(boxes);
        const std::string text((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
        std::remove(boxes.c_str());
        EXPECT_NE(text.find("ct_trace_constructor_target"), std::string::npos) << text;
        EXPECT_NE(text.find("constructor:2"), std::string::npos) << text;
    }
} // namespace
