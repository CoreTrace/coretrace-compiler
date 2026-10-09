// SPDX-License-Identifier: Apache-2.0
// The program's own DLL, as ct_leak_exit_shared_library_lib.cpp: exit-time code that would
// release tracked blocks.
#include "probe_report.h"

static void user_atexit(void)
{
    probe_report("user-dll-atexit");
}

__attribute__((destructor)) static void user_destructor(void)
{
    probe_report("user-dll-destructor");
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    if (reason == DLL_PROCESS_ATTACH)
    {
        probe_write("event=user-dll-attach\n");
        atexit(user_atexit);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        probe_report(reserved != NULL ? "user-dll-detach(process-exit)" : "user-dll-detach");
    }
    return TRUE;
}

__declspec(dllexport) int probe_user_touch(void)
{
    return 1;
}
