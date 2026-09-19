// SPDX-License-Identifier: Apache-2.0
#include "compilerlib/args_internal.hpp"

#include <llvm/Config/llvm-config.h>
#include <llvm/TargetParser/Triple.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
    using Args = std::vector<std::string>;

    llvm::opt::ArgStringList argList(std::initializer_list<const char*> items)
    {
        return llvm::opt::ArgStringList(items.begin(), items.end());
    }

    TEST(FindArgValue, SeparateValue)
    {
        auto args = argList({"-c", "-o", "out.o", "in.c"});
        EXPECT_STREQ(compilerlib::findArgValue(args, "-o"), "out.o");
    }

    TEST(FindArgValue, JoinedWithEquals)
    {
        auto args = argList({"-c", "in.c", "-o=out.o"});
        EXPECT_STREQ(compilerlib::findArgValue(args, "-o"), "out.o");
    }

    TEST(FindArgValue, TrailingSeparateOptionHasNoValue)
    {
        auto args = argList({"-c", "in.c", "-o"});
        EXPECT_EQ(compilerlib::findArgValue(args, "-o"), nullptr);
    }

    TEST(FindArgValue, PrefixWithoutEqualsIsNotAMatch)
    {
        auto args = argList({"-output-file", "x"});
        EXPECT_EQ(compilerlib::findArgValue(args, "-o"), nullptr);
    }

    TEST(HasArg, ExactMatchOnly)
    {
        Args args{"-c", "-fPIE", "-O2"};
        EXPECT_TRUE(compilerlib::hasArg(args, "-fPIE"));
        EXPECT_FALSE(compilerlib::hasArg(args, "-fPI"));
        EXPECT_FALSE(compilerlib::hasArg(args, "-fpie"));
    }

    TEST(RequestsDebugInfo, NoFlagMeansNoDebugInfo)
    {
        EXPECT_FALSE(compilerlib::requestsDebugInfo(Args{"-c", "-O2"}));
    }

    TEST(RequestsDebugInfo, PlainAndLeveledFlagsRequestDebugInfo)
    {
        EXPECT_TRUE(compilerlib::requestsDebugInfo(Args{"-g"}));
        EXPECT_TRUE(compilerlib::requestsDebugInfo(Args{"-g3"}));
        EXPECT_TRUE(compilerlib::requestsDebugInfo(Args{"-gline-tables-only"}));
        EXPECT_TRUE(compilerlib::requestsDebugInfo(Args{"-gdwarf-4"}));
    }

    TEST(RequestsDebugInfo, G0IsNotARequestAndLastLevelWins)
    {
        EXPECT_FALSE(compilerlib::requestsDebugInfo(Args{"-g0"}));
        EXPECT_FALSE(compilerlib::requestsDebugInfo(Args{"-g", "-g0"}));
        EXPECT_TRUE(compilerlib::requestsDebugInfo(Args{"-g0", "-g"}));
    }

    TEST(RequestsDebugInfo, GnoModifiersNeverRequestDebugInfo)
    {
        EXPECT_FALSE(compilerlib::requestsDebugInfo(Args{"-gno-column-info"}));
        EXPECT_TRUE(compilerlib::requestsDebugInfo(Args{"-g", "-gno-column-info"}));
    }

    TEST(EffectiveTargetTriple, DefaultsToTheHostTriple)
    {
        const llvm::Triple expected(llvm::Triple::normalize(LLVM_DEFAULT_TARGET_TRIPLE));
        EXPECT_EQ(compilerlib::effectiveTargetTriple(Args{"-c", "in.c"}).str(), expected.str());
    }

    TEST(EffectiveTargetTriple, HonoursEverySpelling)
    {
        const std::string want = "x86_64-unknown-linux-gnu";
        EXPECT_EQ(compilerlib::effectiveTargetTriple(Args{"-target", want}).str(), want);
        EXPECT_EQ(compilerlib::effectiveTargetTriple(Args{"--target", want}).str(), want);
        EXPECT_EQ(compilerlib::effectiveTargetTriple(Args{"--target=" + want}).str(), want);
        EXPECT_EQ(compilerlib::effectiveTargetTriple(Args{"-target=" + want}).str(), want);
    }

    TEST(EffectiveTargetTriple, LastTargetWins)
    {
        Args args{"-target", "aarch64-unknown-linux-gnu", "--target=x86_64-unknown-linux-gnu"};
        EXPECT_EQ(compilerlib::effectiveTargetTriple(args).str(), "x86_64-unknown-linux-gnu");
    }

    TEST(NormalizeEqualsArgs, SplitsOutputAndLanguage)
    {
        Args args{"-o=app", "-x=c++", "main.cpp"};
        compilerlib::normalizeEqualsArgs(args);
        EXPECT_EQ(args, (Args{"-o", "app", "-x", "c++", "main.cpp"}));
    }

    TEST(NormalizeEqualsArgs, LeavesOtherEqualsArgumentsAlone)
    {
        Args args{"--ct-shadow=aggressive", "-DFOO=1", "-o", "app"};
        const Args before = args;
        compilerlib::normalizeEqualsArgs(args);
        EXPECT_EQ(args, before);
    }

    TEST(MergeDiagnostics, JoinsWithASingleNewline)
    {
        EXPECT_EQ(compilerlib::mergeDiagnostics("driver", "cc1"), "driver\ncc1");
        EXPECT_EQ(compilerlib::mergeDiagnostics("driver\n", "cc1"), "driver\ncc1");
        EXPECT_EQ(compilerlib::mergeDiagnostics("", "cc1"), "cc1");
        EXPECT_EQ(compilerlib::mergeDiagnostics("driver", ""), "driver");
    }

    TEST(IsCc1Command, DetectsTheCc1Marker)
    {
        EXPECT_TRUE(compilerlib::isCc1Command(argList({"clang", "-cc1", "-triple", "x"})));
        EXPECT_FALSE(compilerlib::isCc1Command(argList({"ld", "-o", "app"})));
    }
} // namespace
