// SPDX-License-Identifier: Apache-2.0
// Forks while other threads allocate, release and log. fork() copies only the calling
// thread: a lock another thread held at that moment stays held in the child, where no
// thread releases it. Each child allocates and logs once, then exits; one that has not
// exited after a second is killed and counted as hung. The runtime logs free(NULL) as
// a warning: that is how the fixture writes lines, under the logger's lock, without the
// volume of allocation tracing.
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define THREADS 4
#define FORKS 20
#define CHILD_TIMEOUT_MS 1000

static atomic_int stop;

static void* allocate(void* unused)
{
    (void)unused;
    while (!atomic_load(&stop))
    {
        char* volatile block = malloc(64);
        block[0] = 1;
        free(block);
    }
    return NULL;
}

static void* write_lines(void* unused)
{
    (void)unused;
    while (!atomic_load(&stop))
    {
        free(NULL);
        usleep(20);
    }
    return NULL;
}

// 0 when the child exited on its own, 1 when it had to be killed.
static int reap(pid_t child)
{
    int status = 0;
    for (int waited = 0; waited < CHILD_TIMEOUT_MS; ++waited)
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

int main(void)
{
    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; ++i)
    {
        if (pthread_create(&threads[i], NULL, i == 0 ? write_lines : allocate, NULL) != 0)
        {
            return 2;
        }
    }

    int hung = 0;
    for (int i = 0; i < FORKS; ++i)
    {
        const pid_t child = fork();
        if (child == 0)
        {
            char* volatile block = malloc(32);
            block[0] = 2;
            free(block);
            free(NULL);
            _exit(0);
        }
        if (child < 0)
        {
            return 2;
        }
        hung += reap(child);
    }

    atomic_store(&stop, 1);
    for (int i = 0; i < THREADS; ++i)
    {
        pthread_join(threads[i], NULL);
    }
    printf("forks=%d hung=%d\n", FORKS, hung);
    return 0;
}
