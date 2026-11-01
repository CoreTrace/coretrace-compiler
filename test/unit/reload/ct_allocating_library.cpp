// SPDX-License-Identifier: Apache-2.0
//
// A DLL that links the runtime and allocates through its copy, as an instrumented DLL does: the
// copy records the blocks in its allocation table and its block index, and keeps the blocks it
// frees in its quarantine. runtime_vtable_replace_test.cpp loads it, has it allocate, and unloads
// it, to check what its copy keeps once FreeLibrary has unloaded it (#227). Its allocation trace
// is off, so that many blocks print nothing.
#include <cstddef>

extern "C" void* __ct_malloc(size_t size, const char* site);
extern "C" void __ct_free(void* ptr, const char* site);

extern "C" int __ct_config_disable_alloc_trace = 1;

// Allocates `count` blocks of `size` bytes through the copy of the runtime, then frees them all.
extern "C" __declspec(dllexport) void ct_allocating_library_churn(int count, size_t size)
{
    auto** blocks = static_cast<void**>(__ct_malloc(sizeof(void*) * count, "allocating:array"));
    for (int index = 0; index < count; ++index)
    {
        blocks[index] = __ct_malloc(size, "allocating:block");
    }
    for (int index = 0; index < count; ++index)
    {
        __ct_free(blocks[index], "allocating:block");
    }
    __ct_free(blocks, "allocating:array");
}

// A block of `size` bytes allocated through the copy of the runtime, which the DLL never frees.
extern "C" __declspec(dllexport) void* ct_allocating_library_keep(size_t size)
{
    return __ct_malloc(size, "allocating:kept");
}
