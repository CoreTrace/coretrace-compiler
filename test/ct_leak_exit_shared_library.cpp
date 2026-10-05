// SPDX-License-Identifier: Apache-2.0
// The leak report must come after the exit-time code of shared libraries (#158):
// ct_leak_exit_shared_library_lib.cpp, built into a shared library without --instrument,
// calls back into this program from its global object's destructor (16 bytes) and from
// its destructor function (24 bytes), which release tracked blocks: none is a leak.
#include <cstdio>
#include <cstdlib>

extern "C" void ct_exit_library_register(void (*release_object)(), void (*release_function)());

namespace
{
    void* object_block;
    void* function_block;

    void release_object()
    {
        std::free(object_block);
    }

    void release_function()
    {
        std::free(function_block);
    }
} // namespace

int main()
{
    object_block = std::malloc(16);
    function_block = std::malloc(24);
    ct_exit_library_register(release_object, release_function);
    std::puts("ok");
    return 0;
}
