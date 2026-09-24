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
#include <mutex>

// DbgHelp is single-threaded: every call into it, from any thread, must hold this lock.
// Allocated once and never destroyed, so it stays usable while static destructors and
// the unhandled-exception filter run.
inline std::mutex& ct_dbghelp_mutex()
{
    static std::mutex* mutex = new std::mutex;
    return *mutex;
}

// Loads the process symbols the first time any DbgHelp user needs them. Call with
// ct_dbghelp_mutex() held.
inline void ct_dbghelp_initialize_locked()
{
    static bool initialized = false;
    if (initialized)
    {
        return;
    }
    initialized = true;
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    (void)SymInitialize(GetCurrentProcess(), nullptr, TRUE);
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
    std::lock_guard<std::mutex> dbghelp(ct_dbghelp_mutex());
    if (UnDecorateSymbolName(name, buffer, static_cast<DWORD>(sizeof(buffer)), UNDNAME_COMPLETE) ==
        0)
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
