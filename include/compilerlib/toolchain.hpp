// SPDX-License-Identifier: Apache-2.0
#ifndef COMPILERLIB_TOOLCHAIN_HPP
#define COMPILERLIB_TOOLCHAIN_HPP

#include "compilerlib/attributes.hpp"

#include <string>
#include <vector>

namespace compilerlib
{

    struct DriverConfig
    {
        std::string clang_path;
        std::string resource_dir;
        std::string sysroot;
        bool add_resource_dir = false;
        bool add_sysroot = false;
        bool force_cxx_driver = false;
    };

    CT_NODISCARD bool resolveDriverConfig(const std::vector<std::string>& args, DriverConfig& out,
                                          std::string& error);

    struct RuntimeArchives
    {
        std::string runtime;
        std::string logger;
    };

    // Locates the instrumentation runtime archives linked into instrumented programs.
    // Order: CT_RUNTIME_LIB_DIR, then <executable dir>/../lib (installed layout), then
    // the executable's own directory, then the build-tree paths recorded at configure
    // time. A directory is accepted only if it holds both archives.
    CT_NODISCARD bool resolveRuntimeArchives(RuntimeArchives& out, std::string& error);

} // namespace compilerlib

#endif // COMPILERLIB_TOOLCHAIN_HPP
