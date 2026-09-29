// SPDX-License-Identifier: Apache-2.0
//
// Toolchain decisions shared by the driver setup and its unit tests. Internal to
// compilerlib: not installed, not part of the public API.
#ifndef COMPILERLIB_TOOLCHAIN_INTERNAL_HPP
#define COMPILERLIB_TOOLCHAIN_INTERNAL_HPP

#include "compilerlib/attributes.hpp"

#include <string>

namespace compilerlib
{
    // The clang path the driver is created with. Compiling never runs it, but the driver
    // derives its installation directory from it: the clang found, when there is one;
    // otherwise the path the standard LLVM layout puts next to the resource directory,
    // <prefix>/lib/clang/<version> giving <prefix>/bin/clang, which need not exist. Empty
    // when neither is known.
    CT_NODISCARD std::string driverClangPath(const std::string& foundClang,
                                             const std::string& resourceDir);
} // namespace compilerlib

#endif // COMPILERLIB_TOOLCHAIN_INTERNAL_HPP
