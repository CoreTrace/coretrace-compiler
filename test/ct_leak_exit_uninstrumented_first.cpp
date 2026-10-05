// SPDX-License-Identifier: Apache-2.0
// The leak report must come after the exit-time code of objects the instrumentation does
// not see (#152): ct_leak_exit_uninstrumented_first_plain.cpp is compiled without
// --instrument, so no constructor schedules the report from it, and is linked first. Its
// global object's destructor (16 bytes) and its destructor function (24 bytes) release,
// through this file, the blocks they allocated through it: none is a leak.
#include <cstdio>
#include <cstdlib>

extern "C" void* ct_exit_allocate(std::size_t size)
{
    return std::malloc(size);
}

extern "C" void ct_exit_release(void* block)
{
    std::free(block);
}

int main()
{
    std::puts("ok");
    return 0;
}
