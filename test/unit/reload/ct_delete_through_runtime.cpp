// SPDX-License-Identifier: Apache-2.0
//
// Linked into a DLL with ct_allocating_library.cpp: the DLL's operator delete frees through its
// copy of the runtime, as the copies of the library's inline functions that an instrumented DLL
// compiles may, which the linker can keep for the runtime too (#197). The blocks that the runtime
// releases for itself, the nodes of its allocation table among them, then reach its entry points.
#include <cstddef>

extern "C" void __ct_free(void* ptr, const char* site);

void operator delete(void* ptr) noexcept
{
    if (ptr)
    {
        __ct_free(ptr, "delete-through-runtime");
    }
}

void operator delete(void* ptr, std::size_t) noexcept
{
    operator delete(ptr);
}
