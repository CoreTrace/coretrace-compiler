// SPDX-License-Identifier: Apache-2.0
#include "compilerlib/toolchain_internal.hpp"

#include <llvm/Support/Path.h>

#include <gtest/gtest.h>

#include <string>

namespace
{
    using compilerlib::driverClangPath;

    // Compared with forward slashes: paths are joined with the host's separator.
    std::string slashed(const std::string& path)
    {
        return llvm::sys::path::convert_to_slash(path);
    }

    TEST(DriverClangPath, TheClangFoundIsUsed)
    {
        EXPECT_EQ(driverClangPath("/opt/llvm/bin/clang-20", "/opt/llvm/lib/clang/20"),
                  "/opt/llvm/bin/clang-20");
    }

    // Without a clang, the driver gets the one an LLVM install would have next to the
    // resource directory, so it finds the same installation directories.
    TEST(DriverClangPath, WithoutClangTheStandardLayoutPlacesItNextToTheResourceDir)
    {
        EXPECT_EQ(slashed(driverClangPath("", "/opt/llvm/lib/clang/20")), "/opt/llvm/bin/clang");
    }

    TEST(DriverClangPath, ATrailingSeparatorDoesNotShiftTheLayout)
    {
        EXPECT_EQ(slashed(driverClangPath("", "/opt/llvm/lib/clang/20/")), "/opt/llvm/bin/clang");
    }

    TEST(DriverClangPath, WithNeitherThereIsNoPath)
    {
        EXPECT_EQ(driverClangPath("", ""), "");
    }

#ifdef _WIN32
    TEST(DriverClangPath, WindowsLayout)
    {
        EXPECT_EQ(slashed(driverClangPath("", "C:\\LLVM\\lib\\clang\\20")), "C:/LLVM/bin/clang");
    }
#endif
} // namespace
