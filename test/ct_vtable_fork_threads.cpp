// SPDX-License-Identifier: Apache-2.0
// Forks while other threads make virtual calls, each of which prints a box and looks the
// names it shows up in the runtime's cache of demangled names. fork() copies only the calling
// thread: a lock another thread held at that moment, that of the log or of the cache, stays
// held in the child, where no thread releases it. Each child makes a virtual call, then exits;
// one that has not exited after a second is killed and counted as hung.
#include <atomic>
#include <cstdio>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
    constexpr int kThreads = 4;
    constexpr int kForks = 20;
    constexpr int kChildTimeoutMs = 1000;

    struct Shape
    {
        virtual ~Shape() = default;
        virtual int sides() const = 0;
    };

    struct Triangle : Shape
    {
        int sides() const override
        {
            return 3;
        }
    };

    struct Square : Shape
    {
        int sides() const override
        {
            return 4;
        }
    };

    std::atomic<bool> stop{false};
    std::atomic<int> total{0};

    int call(const Shape& shape)
    {
        return shape.sides();
    }

    void* callVirtuals(void* argument)
    {
        Triangle triangle;
        Square square;
        const bool odd = argument != nullptr;
        while (!stop.load())
        {
            total.fetch_add(call(odd ? static_cast<const Shape&>(triangle) : square));
            usleep(50);
        }
        return nullptr;
    }

    // 0 when the child exited on its own, 1 when it had to be killed.
    int reap(pid_t child)
    {
        int status = 0;
        for (int waited = 0; waited < kChildTimeoutMs; ++waited)
        {
            if (waitpid(child, &status, WNOHANG) == child)
            {
                return 0;
            }
            usleep(1000);
        }
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        return 1;
    }
} // namespace

int main()
{
    pthread_t threads[kThreads];
    for (int i = 0; i < kThreads; ++i)
    {
        void* odd = (i % 2 != 0) ? &threads[i] : nullptr;
        if (pthread_create(&threads[i], nullptr, callVirtuals, odd) != 0)
        {
            return 2;
        }
    }

    int hung = 0;
    for (int i = 0; i < kForks; ++i)
    {
        const pid_t child = fork();
        if (child == 0)
        {
            Triangle triangle;
            _exit(call(triangle) == 3 ? 0 : 1);
        }
        if (child < 0)
        {
            return 2;
        }
        hung += reap(child);
    }

    stop.store(true);
    for (pthread_t thread : threads)
    {
        pthread_join(thread, nullptr);
    }
    std::printf("forks=%d hung=%d\n", kForks, hung);
    return 0;
}
