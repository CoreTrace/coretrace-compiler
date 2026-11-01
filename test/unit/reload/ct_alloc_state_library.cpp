// SPDX-License-Identifier: Apache-2.0
//
// Probe of #227, not to merge. A DLL that links the runtime and allocates through its copy, as
// an instrumented DLL does: the copy records the blocks in its allocation table and its block
// index, and keeps the blocks it frees in its quarantine. Its allocation trace is off, so that
// many blocks print nothing.
#include <cstddef>

extern "C" void* __ct_malloc(size_t size, const char* site);
extern "C" void __ct_free(void* ptr, const char* site);

extern "C" int __ct_config_disable_alloc_trace = 1;

// Allocates `count` blocks of `size` bytes through the copy of the runtime, then frees them all.
extern "C" __declspec(dllexport) void ct_alloc_state_churn(int count, size_t size)
{
    auto** blocks = static_cast<void**>(__ct_malloc(sizeof(void*) * count, "alloc_state:array"));
    for (int index = 0; index < count; ++index)
    {
        blocks[index] = __ct_malloc(size, "alloc_state:block");
    }
    for (int index = 0; index < count; ++index)
    {
        __ct_free(blocks[index], "alloc_state:block");
    }
    __ct_free(blocks, "alloc_state:array");
}
