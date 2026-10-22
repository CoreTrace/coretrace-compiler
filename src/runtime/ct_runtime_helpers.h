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
#include <wchar.h>

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

// DbgHelp keeps the modules it listed when it was initialised: a DLL unloaded, then replaced
// by another one at the same base, kept the first one's symbols (#208). Makes the module
// DbgHelp has at `address` the one loaded there now, the same file built at the same time.
// Call with ct_dbghelp_mutex() held, after ct_dbghelp_initialize_locked().
inline void ct_dbghelp_sync_module_locked(const void* address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module))
    {
        return;
    }
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH)
    {
        return;
    }
    const auto* image = reinterpret_cast<const BYTE*>(module);
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
    const DWORD64 base = reinterpret_cast<DWORD64>(module);

    HANDLE process = GetCurrentProcess();
    IMAGEHLP_MODULEW64 known = {};
    known.SizeOfStruct = sizeof(known);
    if (SymGetModuleInfoW64(process, base, &known) != FALSE)
    {
        if (known.BaseOfImage == base && known.TimeDateStamp == headers->FileHeader.TimeDateStamp &&
            _wcsicmp(known.ImageName, path) == 0)
        {
            return;
        }
        (void)SymUnloadModule64(process, known.BaseOfImage);
    }
    (void)SymLoadModuleExW(process, nullptr, path, nullptr, base,
                           headers->OptionalHeader.SizeOfImage, nullptr, 0);
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
