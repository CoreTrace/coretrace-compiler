// SPDX-License-Identifier: Apache-2.0
// A helper DLL the runtime could ship, to report from its detach: does it detach after the
// program's DLLs, and can it still run the executable's report code then?
#include "probe_report.h"

typedef void (*probe_exe_report_fn)(const char*);

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    if (reason == DLL_PROCESS_ATTACH)
    {
        probe_write("event=helper-dll-attach\n");
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        probe_report(reserved != NULL ? "helper-dll-detach(process-exit)" : "helper-dll-detach");
        probe_exe_report_fn report = (probe_exe_report_fn)(void*)GetProcAddress(
            GetModuleHandleW(NULL), "probe_exe_report");
        if (report != NULL)
        {
            report("helper-dll-detach->exe-code");
        }
    }
    return TRUE;
}

__declspec(dllexport) int probe_helper_touch(void)
{
    return 1;
}
