// SPDX-License-Identifier: Apache-2.0
// Self-checking consumer of compile_c: argument validation and buffer bounds.
// Exits 0 and prints "compile_c checks: ok" when every check passes.
#include "compilerlib/compiler_c.h"

#include <stdio.h>
#include <string.h>

#define GUARD 0x7f

static int failures = 0;

static void check(int condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "compile_c checks: FAIL %s\n", what);
        failures++;
    }
}

static int guard_intact(const char *buffer, size_t from, size_t to)
{
    for (size_t i = from; i < to; ++i) {
        if (buffer[i] != (char)GUARD)
            return 0;
    }
    return 1;
}

int main(void)
{
    /* A compile that must fail and produce a diagnostic longer than 16 bytes. */
    const char *failing_args[] = {"-c", "compile_c_checks_missing_input.c"};
    const int failing_argc = (int)(sizeof(failing_args) / sizeof(failing_args[0]));
    char buffer[64];

    /* 1. Null buffer: rejected, no crash. */
    check(compile_c(failing_argc, failing_args, NULL, 16) == 0, "null buffer is rejected");

    /* 2. Zero and negative sizes: rejected and the buffer is not written. */
    memset(buffer, GUARD, sizeof(buffer));
    check(compile_c(failing_argc, failing_args, buffer, 0) == 0, "size 0 is rejected");
    check(guard_intact(buffer, 0, sizeof(buffer)), "size 0 leaves the buffer untouched");
    check(compile_c(failing_argc, failing_args, buffer, -5) == 0, "negative size is rejected");
    check(guard_intact(buffer, 0, sizeof(buffer)), "negative size leaves the buffer untouched");

    /* 3. Invalid argv with a one-byte buffer: only the terminator is written. */
    memset(buffer, GUARD, sizeof(buffer));
    check(compile_c(1, NULL, buffer, 1) == 0, "null argv with argc > 0 is rejected");
    check(buffer[0] == '\0', "size 1 writes only the terminator");
    check(guard_intact(buffer, 1, sizeof(buffer)), "size 1 does not write past the buffer");
    check(compile_c(-1, failing_args, buffer, 8) == 0, "negative argc is rejected");

    /* 4. Diagnostics longer than the buffer are truncated with a terminator. */
    memset(buffer, GUARD, sizeof(buffer));
    check(compile_c(failing_argc, failing_args, buffer, 16) == 0, "missing input fails");
    check(memchr(buffer, '\0', 16) != NULL, "truncated diagnostics are NUL-terminated");
    check(strlen(buffer) > 0, "truncated diagnostics are not empty");
    check(guard_intact(buffer, 16, sizeof(buffer)), "truncation does not write past the buffer");

    /* 5. A large buffer receives the diagnostic that names the missing input. */
    char large[4096];
    check(compile_c(failing_argc, failing_args, large, (int)sizeof(large)) == 0,
          "missing input fails with a large buffer");
    check(strstr(large, "compile_c_checks_missing_input.c") != NULL,
          "diagnostics name the missing input");

    if (failures != 0)
        return 1;
    puts("compile_c checks: ok");
    return 0;
}
