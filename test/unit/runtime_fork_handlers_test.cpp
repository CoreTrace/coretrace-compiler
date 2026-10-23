// SPDX-License-Identifier: Apache-2.0
//
// fork() runs the prepare handlers in the reverse order of their registration, and the parent
// and child handlers in that order: the handlers registered before the runtime's run while the
// forking thread holds every lock of the runtime, in both processes, and whatever they call
// into the runtime must not wait for those locks (#224). This file's initialization registers
// its handlers before the runtime's constructor registers its own, in a program of its own.
// POSIX only.
#include "ct_runtime_internal.h"

#include <gtest/gtest.h>

#include <atomic>

#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
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

extern "C"
{
    void __ct_vcall_trace(void* this_ptr, void* target, const char* site, const char* static_type);
}

namespace
{
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

    // Set in a child of the test only, before the fork whose handlers call into the runtime.
    std::atomic<bool> armed{false};
    // The handlers of this process that ran while the runtime held its locks.
    std::atomic<int> handlers_under_the_locks{0};

    // A virtual call traced, and a block allocated and released, as an instrumented handler
    // would: between them, they take each lock of the runtime that Linux and macOS share.
    void callIntoTheRuntime()
    {
        if (!armed.load())
        {
            return;
        }
        if (ct_runtime_locked_for_fork())
        {
            handlers_under_the_locks.fetch_add(1);
        }
        __ct_vcall_trace(&local, reinterpret_cast<void*>(&describeLocal), "atfork:1", "<unknown>");
        __ct_free(__ct_malloc(16, "atfork:2"), "atfork:3");
    }

    // Registered by this file's initialization, which runs before the runtime's constructor.
    const bool handlers_registered =
        pthread_atfork(callIntoTheRuntime, callIntoTheRuntime, callIntoTheRuntime) == 0;

    // The exit status of the child within ten seconds, or -1: it is killed then, with its
    // group, so that a grandchild that never returns does not outlive the test.
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
        kill(-child, SIGKILL);
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        return -1;
    }

    TEST(ForkHandlers, AHandlerRegisteredBeforeTheRuntimesCallsIntoIt)
    {
        ASSERT_TRUE(handlers_registered);
        ct_set_enabled(CT_FEATURE_ALLOC | CT_FEATURE_SHADOW | CT_FEATURE_VTABLE_DIAG, 1);

        const pid_t child = fork();
        if (child == 0)
        {
            setpgid(0, 0);
            armed.store(true);
            // The prepare handler runs here, the child handler in the grandchild, and the
            // parent handler here again.
            const pid_t grandchild = fork();
            if (grandchild == 0)
            {
                _exit(handlers_under_the_locks.load() == 2 ? 0 : 3);
            }
            int status = 0;
            if (waitpid(grandchild, &status, 0) != grandchild || !WIFEXITED(status))
            {
                _exit(4);
            }
            if (WEXITSTATUS(status) != 0)
            {
                _exit(WEXITSTATUS(status));
            }
            _exit(handlers_under_the_locks.load() == 2 ? 0 : 5);
        }
        ASSERT_GT(child, 0);
        setpgid(child, child);
        EXPECT_EQ(exitStatusInTime(child), 0)
            << "-1: a handler never returned; 3 or 5: a handler ran without the runtime's locks";
    }
} // namespace
