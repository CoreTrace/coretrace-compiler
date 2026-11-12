// SPDX-License-Identifier: Apache-2.0
#include "compilerlib/args_internal.hpp"

#include <llvm/ADT/SmallString.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/Allocator.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/StringSaver.h>
#include <llvm/Support/VirtualFileSystem.h>
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

    // Clang emits no debug information for these options alone: they only change the
    // information another option asks for (#189).
    TEST(RequestsDebugInfo, ModifiersAloneDoNotRequestDebugInfo)
    {
        for (const char* modifier :
             {"-gcolumn-info", "-gsplit-dwarf", "-gz", "-gpubnames", "-gstrict-dwarf"})
        {
            EXPECT_FALSE(compilerlib::requestsDebugInfo(Args{modifier})) << modifier;
            EXPECT_TRUE(compilerlib::requestsDebugInfo(Args{"-g", modifier})) << modifier;
            EXPECT_FALSE(compilerlib::requestsDebugInfo(Args{"-g", "-g0", modifier})) << modifier;
        }
    }

    // Options of Clang's -g group other than levels, which do ask for debug information.
    TEST(RequestsDebugInfo, OtherOptionsOfTheGGroupRequestDebugInfo)
    {
        for (const char* option : {"-ggdb", "-glldb", "-gmodules", "-gused", "-gdwarf"})
        {
            EXPECT_TRUE(compilerlib::requestsDebugInfo(Args{option})) << option;
        }
        EXPECT_FALSE(compilerlib::requestsDebugInfo(Args{"-ggdb0"}));
        EXPECT_FALSE(compilerlib::requestsDebugInfo(Args{"-g", "-ggdb0"}));
    }

    // LLVM 22 put -gcodeview in the -g group: from that version on, it asks for debug
    // information, in CodeView, as -gdwarf does in DWARF.
    TEST(RequestsDebugInfo, CodeViewFollowsTheLlvmVersion)
    {
#if LLVM_VERSION_MAJOR >= 22
        EXPECT_TRUE(compilerlib::requestsDebugInfo(Args{"-gcodeview"}));
#else
        EXPECT_FALSE(compilerlib::requestsDebugInfo(Args{"-gcodeview"}));
#endif
        EXPECT_TRUE(compilerlib::requestsDebugInfo(Args{"-g", "-gcodeview"}));
        EXPECT_FALSE(compilerlib::requestsDebugInfo(Args{"-gcodeview", "-g0"}));
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

    // The directory that `fs` makes of `path`, as normalizeWorkingDirectoryArgs does.
    std::string absoluteIn(llvm::vfs::FileSystem& fs, const char* path)
    {
        llvm::SmallString<256> directory(path);
        EXPECT_FALSE(fs.makeAbsolute(directory));
        return directory.str().str();
    }

    TEST(NormalizeWorkingDirectoryArgs, MakesEverySpellingAbsoluteAndReturnsTheLast)
    {
        auto fs = llvm::vfs::createPhysicalFileSystem();
        Args args{"-working-directory", "one", "-c", "a.c", "-working-directory=two"};
        const std::string last = compilerlib::normalizeWorkingDirectoryArgs(args, *fs);
        EXPECT_EQ(last, absoluteIn(*fs, "two"));
        EXPECT_TRUE(llvm::sys::path::is_absolute(last));
        EXPECT_EQ(args, (Args{"-working-directory", absoluteIn(*fs, "one"), "-c", "a.c",
                              "-working-directory", last}));
    }

    TEST(NormalizeWorkingDirectoryArgs, LeavesTheValuesOfOtherOptionsAlone)
    {
        auto fs = llvm::vfs::createPhysicalFileSystem();
        Args args{"-Xclang", "-working-directory", "-Xclang", "dir", "-c", "a.c"};
        const Args before = args;
        EXPECT_EQ(compilerlib::normalizeWorkingDirectoryArgs(args, *fs), "");
        EXPECT_EQ(args, before);
    }

    // The directory a -cc1 job's relative paths are resolved against, absolute on the host.
    std::string jobDirectory(void)
    {
#ifdef _WIN32
        return "C:\\work";
#else
        return "/work";
#endif
    }

    std::string inJobDirectory(const char* path)
    {
        llvm::SmallString<64> resolved(jobDirectory());
        llvm::sys::path::append(resolved, path);
        return resolved.str().str();
    }

    Args resolveOutputs(const Args& args)
    {
        llvm::opt::ArgStringList list;
        for (const std::string& arg : args)
            list.push_back(arg.c_str());
        llvm::BumpPtrAllocator allocator;
        llvm::StringSaver saver(allocator);
        const llvm::opt::ArgStringList resolved =
            compilerlib::resolveCc1OutputPaths(list, jobDirectory(), [&](llvm::StringRef argument)
                                               { return saver.save(argument).data(); });
        return Args(resolved.begin(), resolved.end());
    }

    TEST(ResolveCc1OutputPaths, ResolvesTheFilesTheJobWrites)
    {
        const Args args{"-cc1",    "-o",
                        "out/a.o", "-dependency-file",
                        "a.d",     "-opt-record-file",
                        "a.yaml",  "-split-dwarf-output",
                        "a.dwo",   "-stack-usage-file",
                        "a.su",    "-serialize-diagnostic-file",
                        "a.dia",   "-header-include-file",
                        "a.h.txt", "-ftime-trace=a.json"};
        EXPECT_EQ(resolveOutputs(args),
                  (Args{"-cc1", "-o", inJobDirectory("out/a.o"), "-dependency-file",
                        inJobDirectory("a.d"), "-opt-record-file", inJobDirectory("a.yaml"),
                        "-split-dwarf-output", inJobDirectory("a.dwo"), "-stack-usage-file",
                        inJobDirectory("a.su"), "-serialize-diagnostic-file",
                        inJobDirectory("a.dia"), "-header-include-file", inJobDirectory("a.h.txt"),
                        "-ftime-trace=" + inJobDirectory("a.json")}));
    }

    // The inputs, which the job resolves itself; the name of the split DWARF file, which
    // the object records as given; absolute paths; and "-", a standard stream.
    TEST(ResolveCc1OutputPaths, LeavesEverythingElseAlone)
    {
        const Args args{"-cc1",
                        "-I",
                        "inc",
                        "-split-dwarf-file",
                        "a.dwo",
                        "-dependency-file",
                        "-",
                        "-o",
                        inJobDirectory("elsewhere.o"),
                        "-x",
                        "c",
                        "a.c"};
        EXPECT_EQ(resolveOutputs(args), args);
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
