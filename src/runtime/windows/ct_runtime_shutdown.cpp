// SPDX-License-Identifier: Apache-2.0
//
// Whether the process that holds this copy of the runtime is terminating, for the code that runs
// while the module detaches: the leak report of a DLL's copy, and the release of its DbgHelp
// session and of its names. That code cannot tell from lpvReserved: only DllMain receives it
// non-null at process termination, the TLS callbacks receive it null then too (#226, #232).
// ct_dll_unloading_dynamically tells whether that code frees what the copy holds. What these
// answers need from the loader is read when the module attaches, so that the code that runs at
// detach does not ask the loader for it (#231).
#include "ct_runtime_internal.h"

#include <atomic>

// The module that holds this copy of the runtime.
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
    // RtlDllShutdownInProgress, which ntdll.dll exports and no SDK header declares (Windows 10 and
    // later), looked up by this file's TLS callback when the module attaches: a lookup at detach
    // takes a lock of the loader that process termination can leave held, as one reported for
    // Microsoft's WIL library (issue 567 there).
    using CtShutdownInProgress = BOOLEAN(NTAPI*)();
    std::atomic<CtShutdownInProgress> ct_shutdown_in_progress{nullptr};
    // The executable's module, read by the same TLS callback.
    std::atomic<HMODULE> ct_executable{nullptr};

    CT_NOINSTR void NTAPI ct_shutdown_tls(PVOID, DWORD reason, PVOID)
    {
        if (reason == DLL_PROCESS_ATTACH)
        {
            ct_executable.store(GetModuleHandleW(nullptr), std::memory_order_release);
            ct_shutdown_in_progress.store(
                reinterpret_cast<CtShutdownInProgress>(
                    GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlDllShutdownInProgress")),
                std::memory_order_release);
        }
    }
} // namespace

CT_NODISCARD CT_NOINSTR CtProcessShutdown ct_process_shutdown(void)
{
    const CtShutdownInProgress shutdown_in_progress =
        ct_shutdown_in_progress.load(std::memory_order_acquire);
    if (!shutdown_in_progress)
    {
        return CtProcessShutdown::Unknown;
    }
    return shutdown_in_progress() ? CtProcessShutdown::InProgress
                                  : CtProcessShutdown::NotInProgress;
}

CT_NODISCARD CT_NOINSTR HMODULE ct_executable_module(void)
{
    return ct_executable.load(std::memory_order_acquire);
}

CT_NODISCARD CT_NOINSTR bool ct_dll_unloading_dynamically(void)
{
    return reinterpret_cast<HMODULE>(&__ImageBase) != ct_executable_module() &&
           ct_process_shutdown() == CtProcessShutdown::NotInProgress;
}

// The TLS callback that reads the executable's module and looks RtlDllShutdownInProgress up, in
// the file of the functions that use them, so that a module calling one links it.
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:ct_shutdown_tls_callback")
#pragma section(".CRT$XLY", read)
extern "C" __declspec(allocate(".CRT$XLY")) __attribute__((used))
const PIMAGE_TLS_CALLBACK ct_shutdown_tls_callback = ct_shutdown_tls;
