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

} // namespace compilerlib

#endif // COMPILERLIB_TOOLCHAIN_HPP
