// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <string>

// What a demangler made of a name: decoded, not a name it decodes, or nothing for now, as when it
// did not get a lock in time; a cache of names keeps the first two only.
enum class CtDemangleOutcome
{
    Decoded,
    NotDecodable,
    Unavailable,
};

#if defined(_WIN32)
#include "compilerlib/attributes.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

// DbgHelp is single-threaded for the whole process, whatever the session. Every copy of the
// runtime in the process, the executable's and that of each DLL that links the runtime, calls it
// with this lock held: a semaphore named after the process, "Local\coretrace-dbghelp-<pid>",
// which a program that calls DbgHelp itself takes around its own calls (README, "DbgHelp on
// Windows"). Unlike a mutex, a semaphore is not taken again by the thread that holds it: the
// crash handler of a thread that faulted inside DbgHelp does not call it again. The wait is
// bounded: a lookup that does not get DbgHelp in time names nothing, rather than wait for a
// thread that may itself wait for the loader lock, held by a thread in DllMain that waits for
// DbgHelp. A thread that waited in vain then does not wait again until it gets DbgHelp. This
// copy's handle of the semaphore, and its session, belong to windows/ct_runtime_dbghelp.cpp.
class CtDbgHelpLock
{
  public:
    // How long a lookup waits for DbgHelp, in milliseconds.
    static constexpr DWORD kLookupWait = 500;

    explicit CtDbgHelpLock(DWORD wait);
    ~CtDbgHelpLock();

    CtDbgHelpLock(const CtDbgHelpLock&) = delete;
    CtDbgHelpLock& operator=(const CtDbgHelpLock&) = delete;

    bool owns() const
    {
        return owns_;
    }

  private:
    HANDLE semaphore_;
    bool owns_ = false;
};

// The DbgHelp session of this copy of the runtime, on a handle of the process of its own, as
// SymInitialize's documentation asks of each component that inspects the current process: the
// program's session, on GetCurrentProcess(), and the other copies' keep their modules. Started if
// it was not, by the first lookup or before the crash handler is installed; null when it cannot
// start, and the next call tries again. Call with the DbgHelp lock held, never from the crash
// handler, which uses ct_dbghelp_started_session_locked().
HANDLE ct_dbghelp_session_locked();

// The session if it has started, null otherwise. Call with the DbgHelp lock held.
HANDLE ct_dbghelp_started_session_locked();

// Brings the session's modules up to date with what the loader reported: drops those of the DLLs
// unloaded since the last time and lists those loaded. False when a step failed: its modules then
// do not count as current. Call with the DbgHelp lock held, never from the crash handler, since it
// lists the modules.
CT_NODISCARD bool ct_dbghelp_sync_modules_locked(HANDLE process);

// Whether the session's module at `address` is still the one the loader has there: its modules
// were brought up to date, the loader is still watched, and it has unloaded no DLL at that
// module's base since, which is always true of the executable and of the module that holds this
// copy. Reads only what the loader reported, for the crash handler. Call with the DbgHelp lock
// held.
CT_NODISCARD bool ct_dbghelp_module_current_locked(HANDLE process, DWORD64 address);

// UnDecorateSymbolName, with the DbgHelp lock: Unavailable when the lock was not free in time.
inline CtDemangleOutcome ct_demangle_outcome(const char* name, std::string& out)
{
    // UnDecorateSymbolName returns an undecorated name, such as a C function's, unchanged
    // and reports success, which would present it as decoded. Microsoft C++ names start
    // with '?', and RTTI type descriptor names, which the vtable diagnostics pass here,
    // with ".?".
    if (!name || (name[0] != '?' && !(name[0] == '.' && name[1] == '?')))
    {
        return CtDemangleOutcome::NotDecodable;
    }

    char buffer[1024];
    CtDbgHelpLock dbghelp(CtDbgHelpLock::kLookupWait);
    if (!dbghelp.owns())
    {
        return CtDemangleOutcome::Unavailable;
    }
    if (UnDecorateSymbolName(name, buffer, static_cast<DWORD>(sizeof(buffer)), UNDNAME_COMPLETE) ==
        0)
    {
        return CtDemangleOutcome::NotDecodable;
    }

    out.assign(buffer);
    return CtDemangleOutcome::Decoded;
}

inline bool ct_demangle(const char* name, std::string& out)
{
    return ct_demangle_outcome(name, out) == CtDemangleOutcome::Decoded;
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
