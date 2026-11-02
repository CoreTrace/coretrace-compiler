// SPDX-License-Identifier: Apache-2.0
//
// A DLL that links the runtime, as an instrumented DLL does, and with it the runtime's watch of
// the loader, its DbgHelp session and the DbgHelp lock of the process:
// runtime_vtable_replace_test.cpp makes its copy of the runtime name a virtual call's target, or
// not, then unloads it, when its destructors name one, and loads other DLLs, or has it fault.
#define CT_RUNTIME_LIBRARY_EXPORT extern "C" __declspec(dllexport)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern "C" void __ct_vcall_trace(void* this_ptr, void* target, const char* site,
                                 const char* static_type);

namespace
{
    struct Gadget
    {
        virtual ~Gadget() = default;
        virtual int value() const
        {
            return 2;
        }
    };

    Gadget gadget;
} // namespace

// Exported, so that DbgHelp names it from the export table, with or without debug information.
CT_RUNTIME_LIBRARY_EXPORT int ct_runtime_library_measure(int value)
{
    return gadget.value() + value;
}

namespace
{
    // Declared after `gadget`, so destroyed before it.
    struct Farewell
    {
        ~Farewell()
        {
            __ct_vcall_trace(&gadget, reinterpret_cast<void*>(&ct_runtime_library_measure),
                             "runtime_library:2", "<unknown>");
        }
    };

    Farewell farewell;
} // namespace

CT_RUNTIME_LIBRARY_EXPORT void ct_runtime_library_trace()
{
    __ct_vcall_trace(&gadget, reinterpret_cast<void*>(&ct_runtime_library_measure),
                     "runtime_library:1", "<unknown>");
}

namespace
{
    volatile int faulted = 0;
} // namespace

// Raises an exception that nothing handles, in the DLL's own code: the handler of the executable's
// copy of the runtime reports it, the DLL's copy installing none (#236). The store after the call
// keeps this frame on the stack, which a tail call would leave out.
CT_RUNTIME_LIBRARY_EXPORT void ct_runtime_library_fault()
{
    RaiseException(0xE0000225, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    faulted = 1;
}
