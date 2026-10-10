// SPDX-License-Identifier: Apache-2.0
// The scenario of ct_vtable_diag_mismatch.cpp on Windows: a virtual call traced with a
// target from another module, kernel32.dll, than the object's vtable, in the program.
#include <windows.h>

#include <cstdio>

extern "C" void __ct_vcall_trace(void* this_ptr, void* target, const char* site,
                                 const char* static_type);

struct Base
{
    virtual ~Base() = default;
    virtual int value() const
    {
        return 1;
    }
};

struct Derived : Base
{
    int value() const override
    {
        return 2;
    }
};

int main()
{
    std::puts("ct_vtable_diag_mismatch_windows");

    Derived obj;
    Base* base = &obj;

    void* target = reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTickCount"));
    if (!target)
    {
        std::puts("ct_vtable_diag_mismatch_windows: GetProcAddress failed");
        return 1;
    }

    __ct_vcall_trace(base, target, "ct_vtable_diag_mismatch_windows.cpp:43:5", "Base");

    return 0;
}
