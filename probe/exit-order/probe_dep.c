// SPDX-License-Identifier: Apache-2.0
// A DLL that probe_cxx.dll depends on: the loader initializes it before, detaches it after.
#include "probe_report.h"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    if (reason == DLL_PROCESS_ATTACH)
    {
        probe_write("event=dep-dll-attach\n");
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        probe_report(reserved != NULL ? "dep-dll-detach(process-exit)" : "dep-dll-detach");
    }
    return TRUE;
}

__declspec(dllexport) int probe_dep_touch(void)
{
    return 1;
}
