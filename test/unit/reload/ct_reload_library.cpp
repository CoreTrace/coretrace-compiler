// SPDX-License-Identifier: Apache-2.0
//
// A library that runtime_vtable_reload_test.cpp loads, unloads and replaces with another
// build of it: CT_RELOAD_NAMESPACE names its type and its function, so that each build
// has names of its own.
#if defined(_WIN32)
#include <windows.h>

#define CT_RELOAD_EXPORT extern "C" __declspec(dllexport)
#else
#define CT_RELOAD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace CT_RELOAD_NAMESPACE
{
    struct Widget
    {
        virtual ~Widget() = default;
        virtual int value() const
        {
            return 1;
        }
    };

    Widget widget;

#if defined(_WIN32)
    __declspec(dllexport)
#else
    __attribute__((visibility("default")))
#endif
    int
    describe(int value)
    {
        return widget.value() + value;
    }

#if defined(_WIN32)
    volatile int faulted = 0;

    // Raises an exception that nothing handles, in a frame named after the build, as the
    // export table names it: the crash backtrace of a build loaded at the base of another
    // (#225). The store after the call keeps that frame on the stack, which a tail call
    // would leave out.
    __declspec(dllexport) void fault()
    {
        RaiseException(0xE0000225, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        faulted = 1;
    }
#endif
} // namespace CT_RELOAD_NAMESPACE

CT_RELOAD_EXPORT void* ct_reload_object()
{
    return &CT_RELOAD_NAMESPACE::widget;
}

CT_RELOAD_EXPORT void* ct_reload_target()
{
    return reinterpret_cast<void*>(&CT_RELOAD_NAMESPACE::describe);
}

#if defined(_WIN32)
CT_RELOAD_EXPORT void* ct_reload_fault()
{
    return reinterpret_cast<void*>(&CT_RELOAD_NAMESPACE::fault);
}
#endif
