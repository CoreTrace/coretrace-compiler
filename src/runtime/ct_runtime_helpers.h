#pragma once

#include <cstddef>
#include <cstdlib>
#include <cxxabi.h>
#include <string>

__attribute__((no_instrument_function))
inline bool ct_demangle(const char *name, std::string &out)
{
    if (!name) {
        return false;
    }
    if (!(name[0] == '_' && name[1] == 'Z')) {
        return false;
    }

    int status = 0;
    size_t length = 0;
    char *demangled = abi::__cxa_demangle(name, nullptr, &length, &status);
    if (status == 0 && demangled) {
        out.assign(demangled);
        std::free(demangled);
        return true;
    }
    if (demangled) {
        std::free(demangled);
    }
    return false;
}
