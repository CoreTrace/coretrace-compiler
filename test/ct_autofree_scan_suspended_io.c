// SPDX-License-Identifier: Apache-2.0
// The conservative auto-free scan suspends every other thread while it marks. Here, a
// thread is blocked in the logger, holding its output lock, because stderr is a full pipe
// that only another thread of this program drains, after a delay. Built with the scan
// running every few milliseconds and logging each pass: a scan that writes, or takes a
// lock, while the threads are suspended waits for a thread it suspended, and the program
// hangs. The scan runs on macOS only; elsewhere this program simply completes. Built
// without the trace module: the drainer's own trace would block on the full pipe. On macOS
// with the scan enabled, a run in which no pass logged through the pipe exits with 3: it
// would not have tested anything.
//
// A pass that ran while the allocator was blocked logs once the allocator leaves the
// logger, so main keeps stderr on the pipe until that line arrives. Restoring stderr
// earlier sent the line to the real stderr, and a thread still writing into the pipe when
// stderr is restored gets EPIPE on macOS, with SIGPIPE: neither is a scan failure.
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int pipe_fds[2];
static atomic_int scans_seen;
// Keeps the allocation at -O2, where clang removes a malloc that is freed at once.
static void* volatile allocated;

// Allocation tracing logs this malloc: the logger blocks in write() on the full pipe.
static void* allocate(void* unused)
{
    (void)unused;
    void* block = malloc(32);
    allocated = block;
    free(block);
    return NULL;
}

static void* drain(void* unused)
{
    (void)unused;
    usleep(300 * 1000);
    char buffer[4096 + 1];
    ssize_t got;
    while ((got = read(pipe_fds[0], buffer, sizeof(buffer) - 1)) > 0)
    {
        buffer[got] = '\0';
        for (const char* at = buffer; (at = strstr(at, "scan(periodic) done")) != NULL; ++at)
        {
            atomic_fetch_add(&scans_seen, 1);
        }
    }
    return NULL;
}

int main(void)
{
    const int saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0 || pipe(pipe_fds) != 0)
    {
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);

    // Fill the pipe, then make stderr write into it.
    char chunk[4096];
    memset(chunk, 'x', sizeof(chunk));
    fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK);
    while (write(pipe_fds[1], chunk, sizeof(chunk)) > 0)
    {
    }
    fcntl(pipe_fds[1], F_SETFL, 0);
    dup2(pipe_fds[1], STDERR_FILENO);

    pthread_t drainer;
    pthread_t allocator;
    pthread_create(&drainer, NULL, drain, NULL);
    pthread_create(&allocator, NULL, allocate, NULL);
    pthread_join(allocator, NULL);

    // Up to 5 s for the line of a pass to come through the drained pipe.
    for (int waited_ms = 0; atomic_load(&scans_seen) == 0 && waited_ms < 5000; ++waited_ms)
    {
        usleep(1000);
    }
    dup2(saved_stderr, STDERR_FILENO);
    close(pipe_fds[1]);
    pthread_join(drainer, NULL);
    printf("scan passes logged while the allocator was blocked: %d\n", atomic_load(&scans_seen));
#if defined(__APPLE__)
    if (getenv("CT_AUTOFREE_SCAN") != NULL && atomic_load(&scans_seen) == 0)
    {
        return 3;
    }
#endif
    return 0;
}
