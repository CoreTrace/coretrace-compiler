// SPDX-License-Identifier: Apache-2.0
//
// A DLL that links the runtime, as an instrumented DLL does, and allocates through its copy of
// the runtime (#232): it keeps one block until the process exits, and releases three from its
// exit-time code, a global object's destructor, an atexit handler and a destructor function. Its
// copy's leak report must list the first block alone. dll_leak_report_main.cpp loads it.
#include <cstddef>
#include <cstdlib>

extern "C" void* __ct_malloc(size_t size, const char* site);
extern "C" void __ct_free(void* ptr, const char* site);

namespace
{
    // Reachable, so that nothing releases it before the report.
    void* volatile kept = nullptr;
    void* released_by_object = nullptr;
    void* released_by_atexit = nullptr;
    void* released_by_function = nullptr;

    struct Holder
    {
        ~Holder()
        {
            __ct_free(released_by_object, "dll_leak:free");
        }
    };

    Holder holder;

    void releaseAtExit()
    {
        __ct_free(released_by_atexit, "dll_leak:free");
    }

    __attribute__((destructor)) void releaseFromDestructorFunction()
    {
        __ct_free(released_by_function, "dll_leak:free");
    }
} // namespace

extern "C" __declspec(dllexport) void ct_leak_dll_allocate()
{
    kept = __ct_malloc(32, "dll_leak:kept");
    released_by_object = __ct_malloc(16, "dll_leak:object");
    released_by_atexit = __ct_malloc(24, "dll_leak:atexit");
    released_by_function = __ct_malloc(40, "dll_leak:function");
    std::atexit(releaseAtExit);
}
