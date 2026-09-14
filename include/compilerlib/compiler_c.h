/* SPDX-License-Identifier: Apache-2.0 */
#ifndef COMPILERLIB_COMPILER_C_H
#define COMPILERLIB_COMPILER_C_H

#ifdef __cplusplus
extern "C"
{
#endif

    /*
     * C entry point of compilerlib.
     *
     * argv holds the driver arguments without the program name; "--in-mem",
     * "--in-memory" and "--instrument" are interpreted here, everything else is
     * forwarded to the compiler. Diagnostics are copied into output_buffer,
     * truncated to fit and always NUL-terminated.
     *
     * Returns 1 on success and 0 on failure. Invalid arguments (a null
     * output_buffer, buffer_size <= 0, or a null argv with argc > 0) are
     * reported as failure without compiling anything.
     */
    int compile_c(int argc, const char** argv, char* output_buffer, int buffer_size);

#ifdef __cplusplus
}
#endif

#endif /* COMPILERLIB_COMPILER_C_H */
