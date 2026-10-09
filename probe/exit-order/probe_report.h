// SPDX-License-Identifier: Apache-2.0
// Probe of #187, not a test: what a leak report could rely on at each exit-time callback of
// a Windows process. Included by every module of the probe; each keeps its own copy.
#ifndef PROBE_REPORT_H
#define PROBE_REPORT_H

#include <windows.h>
#include <winternl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void probe_write(const char* text)
{
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, (DWORD)strlen(text), &written, NULL);
}

typedef BOOLEAN(NTAPI* probe_is_locked_fn)(PRTL_CRITICAL_SECTION);

// Whether this thread holds the loader lock: the PEB field LoaderLock, at offset 0x110 on
// x64 (undocumented, stable since Windows XP x64).
static int probe_loader_lock_held(void)
{
    probe_is_locked_fn is_locked = (probe_is_locked_fn)(void*)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "RtlIsCriticalSectionLockedByThread");
    PRTL_CRITICAL_SECTION lock =
        *(PRTL_CRITICAL_SECTION*)((BYTE*)NtCurrentTeb()->ProcessEnvironmentBlock + 0x110);
    return is_locked != NULL && is_locked(lock);
}

// The checks named in PROBE_CHECKS, comma-separated, run in each callback after its line,
// one at a time, so that a check that crashes the process does not hide the order:
// - always: loader_lock, this thread holds the loader lock; holder, the thread of the
//   executable that holds probe_cs forever, alive or killed; lock, whether probe_cs could be
//   taken, as the runtime's lock would be by the report;
// - heap: a CRT malloc/free, then a process heap allocation;
// - stdio: a line through this module's CRT stdio.
static int probe_check_enabled(const char* check)
{
    char checks[128] = {0};
    if (GetEnvironmentVariableA("PROBE_CHECKS", checks, sizeof checks) == 0)
    {
        return 0;
    }
    return strstr(checks, check) != NULL;
}

static void probe_report(const char* event)
{
    HMODULE exe = GetModuleHandleW(NULL);
    CRITICAL_SECTION* lock = (CRITICAL_SECTION*)(void*)GetProcAddress(exe, "probe_cs");
    HANDLE* holder = (HANDLE*)(void*)GetProcAddress(exe, "probe_holder");
    const char* holder_state = "none";
    if (holder != NULL && *holder != NULL)
    {
        holder_state = WaitForSingleObject(*holder, 0) == WAIT_TIMEOUT ? "alive" : "killed";
    }
    const char* lock_state = "none";
    if (lock != NULL)
    {
        if (TryEnterCriticalSection(lock))
        {
            lock_state = "free";
            LeaveCriticalSection(lock);
        }
        else
        {
            lock_state = "held";
        }
    }
    char line[256];
    _snprintf_s(line, sizeof line, _TRUNCATE, "event=%s loader_lock=%d holder=%s lock=%s\n",
                event, probe_loader_lock_held(), holder_state, lock_state);
    probe_write(line);
    if (probe_check_enabled("heap"))
    {
        probe_write("  heap: CRT malloc\n");
        free(malloc(64));
        probe_write("  heap: process heap\n");
        HeapFree(GetProcessHeap(), 0, HeapAlloc(GetProcessHeap(), 0, 64));
        probe_write("  heap: ok\n");
    }
    if (probe_check_enabled("stdio"))
    {
        probe_write("  stdio: fprintf\n");
        fprintf(stderr, "  stdio: ok\n");
        fflush(stderr);
    }
}

#endif
