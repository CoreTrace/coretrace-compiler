// SPDX-License-Identifier: Apache-2.0
// With CT_BACKTRACE=1, the crash handler must let the process end when stderr is full. A
// child points stderr at a non-blocking pipe that it fills and nobody reads, then crashes:
// its report cannot be written, and it must still exit with 128 + SIGSEGV. A child that has
// not exited after three seconds is killed and reported as hung. The runtime reads
// CT_BACKTRACE when the process starts, so the program first runs itself again with it set.
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHILD_TIMEOUT_MS 3000

static void crash_with_full_stderr(void)
{
    int fds[2];
    if (pipe(fds) != 0)
    {
        _exit(2);
    }
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
    dup2(fds[1], STDERR_FILENO);
    char block[4096];
    memset(block, 'x', sizeof(block));
    while (write(STDERR_FILENO, block, sizeof(block)) > 0)
    {
    }
    // fds[0] stays open, unread: stderr is full, not closed.
    raise(SIGSEGV);
    _exit(3);
}

int main(int argc, char** argv)
{
    (void)argc;
    if (getenv("CT_BACKTRACE") == NULL)
    {
        setenv("CT_BACKTRACE", "1", 1);
        execv(argv[0], argv);
        perror("execv");
        return 2;
    }

    const pid_t child = fork();
    if (child == 0)
    {
        crash_with_full_stderr();
    }
    if (child < 0)
    {
        return 2;
    }
    int status = 0;
    for (int waited = 0; waited < CHILD_TIMEOUT_MS; ++waited)
    {
        if (waitpid(child, &status, WNOHANG) == child)
        {
            if (WIFEXITED(status))
            {
                printf("crash status=%d\n", WEXITSTATUS(status));
            }
            else
            {
                printf("crash signal=%d\n", WIFSIGNALED(status) ? WTERMSIG(status) : -1);
            }
            return 0;
        }
        usleep(1000);
    }
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    printf("crash hung\n");
    return 0;
}
