// SPDX-License-Identifier: Apache-2.0
// The instrumented program: the runtime is linked into it statically, and the report is
// today a .CRT$XTY terminator. Candidate later points: a TLS callback, an FLS callback, and
// the detach of a helper DLL (probe_helper.c). Arguments: return|exit|exitprocess [churn].
#include "probe_report.h"

__declspec(dllimport) int probe_user_touch(void);
__declspec(dllimport) int probe_helper_touch(void);

// Held forever by `holder`, which ExitProcess kills: the runtime's lock, taken by a thread
// at the moment the process exits.
__declspec(dllexport) CRITICAL_SECTION probe_cs;
__declspec(dllexport) HANDLE probe_holder;

__declspec(dllexport) void probe_exe_report(const char* event)
{
    probe_report(event);
}

static DWORD WINAPI hold_lock(LPVOID unused)
{
    (void)unused;
    EnterCriticalSection(&probe_cs);
    Sleep(INFINITE);
    return 0;
}

// Allocates and releases without pause, as instrumented threads do while the process exits.
static DWORD WINAPI churn(LPVOID unused)
{
    (void)unused;
    for (;;)
    {
        free(malloc(32));
        HeapFree(GetProcessHeap(), 0, HeapAlloc(GetProcessHeap(), 0, 48));
    }
    return 0;
}

static void exe_atexit(void)
{
    probe_report("exe-atexit");
}

static void exe_xty(void)
{
    probe_report("exe-crt-xty(report today)");
}
#pragma section(".CRT$XTY", read)
__declspec(allocate(".CRT$XTY")) __attribute__((used)) static void (*const probe_xty)(void) =
    exe_xty;

static void NTAPI exe_tls_callback(PVOID module, DWORD reason, PVOID reserved)
{
    (void)module;
    if (reason == DLL_PROCESS_DETACH)
    {
        probe_report(reserved != NULL ? "exe-tls-detach(process-exit)" : "exe-tls-detach");
    }
}
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:probe_tls_callback")
#pragma section(".CRT$XLB", read)
__declspec(allocate(".CRT$XLB")) __attribute__((used)) const PIMAGE_TLS_CALLBACK
    probe_tls_callback = exe_tls_callback;

static VOID WINAPI exe_fls_callback(PVOID data)
{
    (void)data;
    probe_report("exe-fls-callback");
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "return";
    probe_write("event=main\n");
    InitializeCriticalSection(&probe_cs);
    probe_holder = CreateThread(NULL, 0, hold_lock, NULL, 0, NULL);
    if (argc > 2 && strcmp(argv[2], "churn") == 0)
    {
        for (int i = 0; i < 3; ++i)
        {
            CreateThread(NULL, 0, churn, NULL, 0, NULL);
        }
    }
    Sleep(50);
    DWORD fls = FlsAlloc(exe_fls_callback);
    FlsSetValue(fls, (PVOID)1);
    atexit(exe_atexit);
    probe_user_touch();
    probe_helper_touch();
    probe_write("event=main-ends\n");
    if (strcmp(mode, "exit") == 0)
    {
        exit(0);
    }
    if (strcmp(mode, "exitprocess") == 0)
    {
        ExitProcess(0);
    }
    return 0;
}
