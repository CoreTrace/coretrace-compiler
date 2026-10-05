// SPDX-License-Identifier: Apache-2.0
// Compiled without --instrument for ct_leak_exit_uninstrumented_first.cpp, and linked
// before it. Its exit-time code releases blocks through the instrumented program.
#include <cstddef>

extern "C" void* ct_exit_allocate(std::size_t size);
extern "C" void ct_exit_release(void* block);

namespace
{
    struct Holder
    {
        void* block = ct_exit_allocate(16);
        ~Holder()
        {
            ct_exit_release(block);
        }
    };

    Holder holder;
    void* function_block;

    __attribute__((constructor)) void allocate_for_function()
    {
        function_block = ct_exit_allocate(24);
    }

    __attribute__((destructor)) void release_from_function()
    {
        ct_exit_release(function_block);
    }
} // namespace
