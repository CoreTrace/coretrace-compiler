// SPDX-License-Identifier: Apache-2.0
//
// Linked into a DLL with ct_allocating_library.cpp: the DLL's operators new and delete allocate
// and free through its copy of the runtime, as the copies of the library's inline functions that
// an instrumented DLL compiles may, which the linker can keep for the runtime too (#197). The
// blocks that the runtime allocates and releases for itself, the nodes of its allocation table
// among them, then reach its entry points.
#include <cstddef>
#include <new>

extern "C" void* __ct_malloc(size_t size, const char* site);
extern "C" void __ct_free(void* ptr, const char* site);

void* operator new(std::size_t size)
{
    if (void* ptr = __ct_malloc(size, "new-through-runtime"))
    {
        return ptr;
    }
    throw std::bad_alloc();
}

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
