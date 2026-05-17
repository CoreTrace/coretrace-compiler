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

inline bool ct_demangle(const char* name, std::string& out)
{
    if (!name || name[0] == '\0')
    {
        return false;
    }

    char buffer[1024];
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
