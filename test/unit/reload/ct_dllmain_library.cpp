// SPDX-License-Identifier: Apache-2.0
//
// A DLL whose constructor, which runs while LoadLibrary holds the loader lock, calls the
// program's ct_test_constructor_hook: runtime_vtable_replace_test.cpp names a virtual call from
// there while another of its threads names one too. It does not link the runtime.
#include <windows.h>

namespace
{
    struct Widget
    {
        virtual ~Widget() = default;
        virtual int value() const
        {
            return 4;
        }
    };

    Widget widget;
} // namespace

// Exported, so that DbgHelp names it from the export table, with or without debug information.
extern "C" __declspec(dllexport) int ct_dllmain_measure(int value)
{
    return widget.value() + value;
}

namespace
{
    // Declared after `widget`, so constructed after it.
    struct Constructor
    {
        Constructor()
        {
            using Hook = void (*)(void*, void*);
            auto* hook = reinterpret_cast<Hook>(
                GetProcAddress(GetModuleHandleW(nullptr), "ct_test_constructor_hook"));
            if (hook)
            {
                hook(&widget, reinterpret_cast<void*>(&ct_dllmain_measure));
            }
        }
    };

    Constructor constructor;
} // namespace
