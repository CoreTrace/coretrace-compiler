// SPDX-License-Identifier: Apache-2.0
// Several threads enter a traced C++ function at once, so the runtime decodes its name
// concurrently: through DbgHelp on Windows, which must serialise every call.
#include <atomic>
#include <thread>
#include <vector>

static std::atomic<int> calls{0};

int work(int value)
{
    calls.fetch_add(1, std::memory_order_relaxed);
    return value + 1;
}

int main()
{
    constexpr int kThreads = 4;
    constexpr int kCallsPerThread = 100;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(
            []
            {
                for (int i = 0; i < kCallsPerThread; ++i)
                {
                    (void)work(i);
                }
            });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    return calls.load() == kThreads * kCallsPerThread ? 0 : 1;
}
