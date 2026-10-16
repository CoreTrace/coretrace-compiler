// SPDX-License-Identifier: Apache-2.0
//
// How the auto-free scans stop the other threads (#209). The roots a scan read are all the
// threads' roots only when every thread of the process is one it listed and suspended, and no
// thread started since it listed them; otherwise it must release nothing. And how the hook that
// counts thread starts passes every event on to the hook installed before it.
//
// macOS only, where the scans exist. An executable of its own, run with CT_AUTOFREE_SCAN=1:
// enabling the scan installs the hook that counts thread starts.
#include "ct_runtime_alloc_internal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <mach/mach.h>

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
    using namespace std::chrono_literals;

    // A thread that waits, without taking any lock, until the object goes. It has started
    // once the constructor returns: a thread that starts after the list is made counts as one
    // the scan may not have read.
    class WaitingThread
    {
      public:
        WaitingThread()
            : thread_(
                  [this]
                  {
                      started_.store(true);
                      while (!stop_.load())
                      {
                          std::this_thread::sleep_for(1ms);
                      }
                  })
        {
            while (!started_.load())
            {
                std::this_thread::yield();
            }
        }

        ~WaitingThread()
        {
            stop_.store(true);
            thread_.join();
        }

      private:
        std::atomic<bool> started_{false};
        std::atomic<bool> stop_{false};
        std::thread thread_;
    };

    // Suspends the listed threads, checks them, and resumes them before the result is
    // asserted: the test reports nothing while threads are suspended.
    bool suspendedThreadsAreComplete(struct ct_scan_threads* threads)
    {
        ct_suspend_threads(threads);
        const bool complete = ct_threads_complete(*threads);
        ct_resume_threads(threads);
        return complete;
    }

    TEST(ScanThreads, AreCompleteWhenNoThreadWasCreatedSinceTheList)
    {
        WaitingThread waiting;
        struct ct_scan_threads threads = {};
        ASSERT_TRUE(ct_list_threads(&threads));

        EXPECT_TRUE(suspendedThreadsAreComplete(&threads));
    }

    // A thread that exists but has not run yet, as one the kernel has just created for a
    // pthread_create: only the second list of the threads sees it.
    TEST(ScanThreads, AreIncompleteWhenAThreadWasCreatedSinceTheList)
    {
        struct ct_scan_threads threads = {};
        ASSERT_TRUE(ct_list_threads(&threads));
        thread_act_t created = MACH_PORT_NULL;
        ASSERT_EQ(thread_create(mach_task_self(), &created), KERN_SUCCESS);

        EXPECT_FALSE(suspendedThreadsAreComplete(&threads));

        EXPECT_EQ(thread_terminate(created), KERN_SUCCESS);
        mach_port_deallocate(mach_task_self(), created);
    }

    // Whether every thread the kernel lists now is in the list. The list holds a send right to
    // each of its threads, so they keep their port names; an ended thread may show for a while
    // under a new name.
    bool kernelListsOnly(const struct ct_scan_threads& threads)
    {
        thread_act_array_t ports = nullptr;
        mach_msg_type_number_t count = 0;
        EXPECT_EQ(task_threads(mach_task_self(), &ports, &count), KERN_SUCCESS);
        bool only = true;
        for (mach_msg_type_number_t i = 0; i < count; ++i)
        {
            bool listed = false;
            for (mach_msg_type_number_t j = 0; j < threads.count; ++j)
            {
                listed = listed || threads.items[j].port == ports[i];
            }
            only = only && listed;
            mach_port_deallocate(mach_task_self(), ports[i]);
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(ports),
                      count * sizeof(thread_t));
        return only;
    }

    // A thread gone before the check could have moved a reference while the scan read the
    // roots: only the count of thread starts sees it. The test waits until the kernel lists
    // no thread but the listed ones, so that the second list cannot see it either.
    TEST(ScanThreads, AreIncompleteWhenAThreadStartedAndEndedSinceTheList)
    {
        struct ct_scan_threads threads = {};
        ASSERT_TRUE(ct_list_threads(&threads));
        std::thread([] {}).join();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!kernelListsOnly(threads) && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(1ms);
        }
        ASSERT_TRUE(kernelListsOnly(threads)) << "the ended thread is still listed";

        EXPECT_FALSE(suspendedThreadsAreComplete(&threads));
    }

    std::atomic<int> g_previous_hook_events{0};

    void previousHook(unsigned int, pthread_t, void*, size_t)
    {
        g_previous_hook_events.fetch_add(1);
    }

    // The scan's hook is live from the exchange that installs it, a moment before the exchange
    // returns the previous hook: an event in that moment must still reach the previous hook.
    TEST(ThreadHookChain, AnEventBeforeTheLinkReachesThePreviousHook)
    {
        g_previous_hook_events.store(0);
        struct ct_thread_hook_chain chain;
        std::atomic<bool> returned{false};
        std::thread event(
            [&]
            {
                ct_thread_hook_chain_forward(chain, PTHREAD_INTROSPECTION_THREAD_START,
                                             pthread_self(), nullptr, 0);
                returned.store(true);
            });
        // Time enough for an event that does not wait for the link to come back without it.
        const auto deadline = std::chrono::steady_clock::now() + 500ms;
        while (!returned.load() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_FALSE(returned.load()) << "the event did not wait for the previous hook";

        ct_thread_hook_chain_link(chain, previousHook);
        event.join();

        EXPECT_EQ(g_previous_hook_events.load(), 1);
    }

    TEST(ThreadHookChain, AnEventWithNoPreviousHookCallsNothing)
    {
        struct ct_thread_hook_chain chain;
        ct_thread_hook_chain_link(chain, nullptr);

        ct_thread_hook_chain_forward(chain, PTHREAD_INTROSPECTION_THREAD_START, pthread_self(),
                                     nullptr, 0);
    }
} // namespace
