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
extern "C" void __ct_check_bounds(const void* base, const void* ptr, size_t access_size,
                                  const char* site, int is_write);

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

namespace
{
    int churn_at_unload_count = 0;
    size_t churn_at_unload_size = 0;
    bool check_at_unload = false;

    void atUnload()
    {
        if (check_at_unload)
        {
            __ct_check_bounds(&check_at_unload, &check_at_unload, sizeof(check_at_unload),
                              "allocating:check", 0);
        }
        if (churn_at_unload_count > 0)
        {
            ct_allocating_library_churn(churn_at_unload_count, churn_at_unload_size);
        }
    }
} // namespace

// Has the DLL allocate `count` blocks of `size` bytes through the copy of the runtime and free them
// when it unloads, after the terminator of the copy: as code that runs after it would.
extern "C" __declspec(dllexport) void ct_allocating_library_churn_at_unload(int count, size_t size)
{
    churn_at_unload_count = count;
    churn_at_unload_size = size;
}

// Has the DLL check an access through the copy of the runtime when it unloads, after the terminator
// of the copy and before the blocks it allocates then: the first use of the copy's allocation table
// once the terminator has freed it.
extern "C" __declspec(dllexport) void ct_allocating_library_check_at_unload()
{
    check_at_unload = true;
}

// .CRT$XTYZ comes after .CRT$XTY, where the copy places its terminator, and before .CRT$XTZ, which
// ends the terminators.
#pragma comment(linker, "/INCLUDE:ct_allocating_library_late_terminator")
#pragma section(".CRT$XTYZ", read)
extern "C" __declspec(allocate(".CRT$XTYZ"))
__attribute__((used)) void (*const ct_allocating_library_late_terminator)() = atUnload;
