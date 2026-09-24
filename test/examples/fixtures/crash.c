// SPDX-License-Identifier: Apache-2.0
// Faults on purpose: an unmapped write raises SIGSEGV on POSIX and an access violation
// on Windows, which the runtime's fatal-error handler must report.
int main(void)
{
    volatile int* null_pointer = 0;
    *null_pointer = 1;
    return 0;
}
