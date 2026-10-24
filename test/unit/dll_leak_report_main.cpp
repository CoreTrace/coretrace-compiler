// SPDX-License-Identifier: Apache-2.0
//
// Loads the DLL of reload/ct_leak_dll.cpp, which allocates through its copy of the runtime, then
// returns from main with the DLL loaded; with the argument "exitprocess" calls ExitProcess
// instead, and with "free" unloads the DLL first (#232). The program links no runtime: the leak
// report it prints, if any, is the DLL's copy's.
#include <windows.h>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    HMODULE dll = LoadLibraryA(CT_LEAK_DLL);
    auto* allocate =
        dll ? reinterpret_cast<void (*)()>(GetProcAddress(dll, "ct_leak_dll_allocate")) : nullptr;
    if (!allocate)
    {
        std::puts("dll_leak_report_main: the DLL did not load");
        return 2;
    }
    allocate();
    const char* how = argc > 1 ? argv[1] : "";
    if (std::strcmp(how, "free") == 0)
    {
        FreeLibrary(dll);
    }
    std::puts("dll_leak_report_main: done");
    std::fflush(stdout);
    if (std::strcmp(how, "exitprocess") == 0)
    {
        ExitProcess(0);
    }
    return 0;
}
