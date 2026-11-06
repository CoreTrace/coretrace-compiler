// SPDX-License-Identifier: Apache-2.0
//
// The driver interfaces of clang_compat.hpp that need none of Clang's frontend: the
// option identifiers and table, and the resource directory lookup. Internal to compilerlib.
#pragma once

#include <llvm/ADT/StringRef.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Option/OptTable.h>

#if LLVM_VERSION_MAJOR >= 22
#include <clang/Options/OptionUtils.h>
#include <clang/Options/Options.h>
#else
#include <clang/Driver/Driver.h>
#include <clang/Driver/Options.h>
#endif

#include <string>

namespace compilerlib::clang_compat
{
    // The driver's option identifiers (OPT_*), which LLVM 22 moved out of the driver.
#if LLVM_VERSION_MAJOR >= 22
    namespace options = clang::options;
#else
    namespace options = clang::driver::options;
#endif

    // The driver's option table, which LLVM 22 moved out of the driver too.
    inline const llvm::opt::OptTable& driverOptTable()
    {
#if LLVM_VERSION_MAJOR >= 22
        return clang::getDriverOptTable();
#else
        return clang::driver::getDriverOptTable();
#endif
    }

    // The resource directory of the clang at clangPath, as that clang computes it.
    inline std::string resourcesPath(llvm::StringRef clangPath)
    {
#if LLVM_VERSION_MAJOR >= 22
        return clang::GetResourcesPath(clangPath);
#else
        return clang::driver::Driver::GetResourcesPath(clangPath);
#endif
    }
} // namespace compilerlib::clang_compat
