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

// One line per event, written with WriteFile, then the same through this module's CRT stdio:
// - loader_lock: this thread holds the loader lock;
// - holder: the thread of the executable that holds probe_cs forever, alive or killed;
// - lock: whether probe_cs can be taken, as the runtime's lock would be by the report;
// - heap: a CRT malloc/free and a process heap allocation complete.
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
    void* block = malloc(64);
    free(block);
    void* heap_block = HeapAlloc(GetProcessHeap(), 0, 64);
    HeapFree(GetProcessHeap(), 0, heap_block);

    char line[256];
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "event=%s loader_lock=%d holder=%s lock=%s heap=%s\n", event,
                probe_loader_lock_held(), holder_state, lock_state,
                block != NULL && heap_block != NULL ? "ok" : "failed");
    probe_write(line);
    const int stdio = fprintf(stderr, "  stdio of %s\n", event);
    fflush(stderr);
    if (stdio < 0)
    {
        probe_write("  stdio failed\n");
    }
}

#endif
