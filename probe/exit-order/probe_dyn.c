// SPDX-License-Identifier: Apache-2.0
// A DLL the program loads with LoadLibrary and never frees.
#include "probe_report.h"

static void dyn_atexit(void)
{
    probe_report("dyn-dll-atexit");
}

__attribute__((destructor)) static void dyn_destructor(void)
{
    probe_report("dyn-dll-destructor");
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    if (reason == DLL_PROCESS_ATTACH)
    {
        probe_write("event=dyn-dll-attach\n");
        atexit(dyn_atexit);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        probe_report(reserved != NULL ? "dyn-dll-detach(process-exit)" : "dyn-dll-detach");
    }
    return TRUE;
}
