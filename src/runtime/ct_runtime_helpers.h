// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <cwchar>

// DbgHelp is single-threaded for the whole process, whatever the session. Every copy of the
// runtime in the process, the executable's and that of each DLL that links the runtime, calls it
// with this lock held: a semaphore named after the process, "Local\coretrace-dbghelp-<pid>",
// which a program that calls DbgHelp itself takes around its own calls (README, "DbgHelp on
// Windows"). Unlike a mutex, a semaphore is not taken again by the thread that holds it: the
// crash handler of a thread that faulted inside DbgHelp does not call it again. The wait is
// bounded: a lookup that does not get DbgHelp in time names nothing, rather than wait for a
// thread that may itself wait for the loader lock, held by a thread in DllMain that waits for
// DbgHelp. A thread that waited in vain then does not wait again until it gets DbgHelp.
class CtDbgHelpLock
{
  public:
    // How long a lookup waits for DbgHelp, in milliseconds.
    static constexpr DWORD kLookupWait = 500;

    explicit CtDbgHelpLock(DWORD wait) : semaphore_(semaphore()), owns_(acquire(wait)) {}

    ~CtDbgHelpLock()
    {
        if (owns_)
        {
            (void)ReleaseSemaphore(semaphore_, 1, nullptr);
        }
    }

    CtDbgHelpLock(const CtDbgHelpLock&) = delete;
    CtDbgHelpLock& operator=(const CtDbgHelpLock&) = delete;

    bool owns() const
    {
        return owns_;
    }

  private:
    // Opened once per copy and never closed, so that it outlives static destructors; null when
    // it cannot be opened, and DbgHelp is not called then.
    static HANDLE semaphore()
    {
        static std::atomic<HANDLE> opened{nullptr};
        HANDLE handle = opened.load(std::memory_order_acquire);
        if (handle)
        {
            return handle;
        }
        wchar_t name[64];
        (void)swprintf(name, sizeof(name) / sizeof(name[0]), L"Local\\coretrace-dbghelp-%lu",
                       GetCurrentProcessId());
        HANDLE created = CreateSemaphoreW(nullptr, 1, 1, name);
        if (!created)
        {
            return nullptr;
        }
        if (!opened.compare_exchange_strong(handle, created, std::memory_order_acq_rel))
        {
            (void)CloseHandle(created);
            return handle;
        }
        return created;
    }

    bool acquire(DWORD wait)
    {
        thread_local bool waited_in_vain = false;
        if (!semaphore_)
        {
            return false;
        }
        const bool acquired =
            WaitForSingleObject(semaphore_, waited_in_vain ? 0 : wait) == WAIT_OBJECT_0;
        waited_in_vain = !acquired;
        return acquired;
    }

    HANDLE semaphore_;
    bool owns_;
};

// The DbgHelp session of this copy of the runtime, on a handle of the process of its own, as
// SymInitialize's documentation asks of each component that inspects the current process: the
// program's session, on GetCurrentProcess(), and the other copies' keep their modules. Null until
// it has started, then never closed. Used with the DbgHelp lock held.
inline HANDLE ct_dbghelp_process = nullptr;

// The session, started if it was not, by the first lookup or before the crash handler is
// installed; null when it cannot start, and the next call tries again. Never from the crash
// handler, which uses ct_dbghelp_process as it is.
inline HANDLE ct_dbghelp_session_locked()
{
    if (ct_dbghelp_process)
    {
        return ct_dbghelp_process;
    }
    HANDLE process = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &process, 0,
                         FALSE, DUPLICATE_SAME_ACCESS))
    {
        return nullptr;
    }
    // DbgHelp's options are the process's: the runtime adds its own to the program's.
    SymSetOptions(SymGetOptions() | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    if (!SymInitialize(process, nullptr, TRUE))
    {
        (void)CloseHandle(process);
        return nullptr;
    }
    ct_dbghelp_process = process;
    return process;
}

inline bool ct_demangle(const char* name, std::string& out)
{
    // UnDecorateSymbolName returns an undecorated name, such as a C function's, unchanged
    // and reports success, which would present it as decoded. Microsoft C++ names start
    // with '?', and RTTI type descriptor names, which the vtable diagnostics pass here,
    // with ".?".
    if (!name || (name[0] != '?' && !(name[0] == '.' && name[1] == '?')))
    {
        return false;
    }

    char buffer[1024];
    CtDbgHelpLock dbghelp(CtDbgHelpLock::kLookupWait);
    if (!dbghelp.owns() || UnDecorateSymbolName(name, buffer, static_cast<DWORD>(sizeof(buffer)),
                                                UNDNAME_COMPLETE) == 0)
    {
        return false;
    }

    out.assign(buffer);
    return true;
}
#else
#include <cstdlib>
#include <cxxabi.h>

__attribute__((no_instrument_function)) inline bool ct_demangle(const char* name, std::string& out)
{
    if (!name)
    {
        return false;
    }
    if (!(name[0] == '_' && name[1] == 'Z'))
    {
        return false;
    }

    int status = 0;
    size_t length = 0;
    char* demangled = abi::__cxa_demangle(name, nullptr, &length, &status);
    if (status == 0 && demangled)
    {
        out.assign(demangled);
        std::free(demangled);
        return true;
    }
    if (demangled)
    {
        std::free(demangled);
    }
    return false;
}
#endif
