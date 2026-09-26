// SPDX-License-Identifier: Apache-2.0
#include "compilerlib/compiler.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/resource.h>
#endif

namespace
{
    namespace fs = std::filesystem;

    // Compiles in this process, as hosts of the compiler library do, with sources and
    // outputs in a directory of the test's own.
    class CompileTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            const ::testing::TestInfo* test =
                ::testing::UnitTest::GetInstance()->current_test_info();
            std::string name = std::string("ct_") + test->test_suite_name() + "." + test->name();
            std::replace(name.begin(), name.end(), '/', '_');
            dir_ = fs::path(::testing::TempDir()) / name;
            fs::remove_all(dir_);
            fs::create_directories(dir_);
        }

        void TearDown() override
        {
            std::error_code ignored;
            fs::remove_all(dir_, ignored);
        }

        std::string path(const char* name) const
        {
            return (dir_ / name).string();
        }

        std::string writeSource(const char* name, const char* text) const
        {
            fs::create_directories(fs::path(path(name)).parent_path());
            std::ofstream(path(name)) << text;
            return path(name);
        }

      private:
        fs::path dir_;
    };

    std::string readFile(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // A local array whose address escapes, so the bounds module registers it.
    constexpr const char* kEscapingArray = R"(void use(int* values);

int main(void)
{
    int values[4] = {0};
    use(values);
    return values[0];
}
)";

    // Module sets that run trace before bounds, the default one ("") included: trace's
    // entry call then comes before the allocas that bounds registers.
    class TraceWithBoundsTest : public CompileTest,
                                public ::testing::WithParamInterface<std::string>
    {
    };

    TEST_P(TraceWithBoundsTest, InstrumentedModuleIsValid)
    {
        std::vector<std::string> args = {"-S", "-emit-llvm",
                                         writeSource("escape.c", kEscapingArray)};
        if (!GetParam().empty())
            args.push_back("--ct-modules=" + GetParam());
        compilerlib::CompileResult result =
            compilerlib::compile(args, compilerlib::OutputMode::ToMemory, /*instrument=*/true);
        ASSERT_TRUE(result.success) << result.diagnostics;

        llvm::LLVMContext context;
        llvm::SMDiagnostic parseError;
        std::unique_ptr<llvm::Module> module =
            llvm::parseAssemblyString(result.llvmIR, parseError, context);
        ASSERT_NE(module, nullptr) << parseError.getMessage().str();
        std::string problems;
        llvm::raw_string_ostream stream(problems);
        EXPECT_FALSE(llvm::verifyModule(*module, &stream)) << stream.str();
    }

    INSTANTIATE_TEST_SUITE_P(ModuleSets, TraceWithBoundsTest,
                             ::testing::Values("", "trace,bounds", "trace,alloc,bounds", "all"),
                             [](const ::testing::TestParamInfo<std::string>& info)
                             {
                                 std::string name = info.param.empty() ? "default" : info.param;
                                 std::replace(name.begin(), name.end(), ',', '_');
                                 return name;
                             });

    // With the default modules, code generation rejected this fixture's invalid IR on
    // macOS arm64.
    TEST_F(CompileTest, DefaultModulesCompileVtableFixture)
    {
        compilerlib::CompileResult result = compilerlib::compile(
            {"-c", CT_TEST_SOURCE_DIR "/ct_vtable_diag_fake.cpp", "-o", path("fake.o")},
            compilerlib::OutputMode::ToFile, /*instrument=*/true);
        EXPECT_TRUE(result.success) << result.diagnostics;
    }

    // LLVM ends the process on a code-generation error that no diagnostic handler takes:
    // compile() must return the failure instead, with the frontend's diagnostics.
    TEST_F(CompileTest, CodeGenerationErrorFailsCompile)
    {
        compilerlib::CompileResult result =
            compilerlib::compile({"-c", CT_TEST_SOURCE_DIR "/examples/fixtures/codegen_error.c",
                                  "-o", path("codegen_error.o")},
                                 compilerlib::OutputMode::ToFile, /*instrument=*/true);
        EXPECT_FALSE(result.success);
        EXPECT_NE(result.diagnostics.find("ct_not_an_instruction"), std::string::npos)
            << result.diagnostics;
        EXPECT_NE(result.diagnostics.find("frontend warning before a code-generation error"),
                  std::string::npos)
            << result.diagnostics;
    }

    // A heap allocation and a registered stack object, in a source under two directories.
    constexpr const char* kSites = R"(void* malloc(__SIZE_TYPE__ size);
void use(int* values);

int main(void)
{
    int values[4] = {0};
    use(values);
    return malloc(4) != 0;
}
)";

    // Sites keep the path the compiler was given, so that files with the same name in
    // different directories stay apart.
    TEST_F(CompileTest, SitesKeepTheSourcePath)
    {
        compilerlib::CompileResult result =
            compilerlib::compile({"-g", "-S", "-emit-llvm", "--ct-modules=alloc,bounds",
                                  writeSource("sub/dir/sites.c", kSites)},
                                 compilerlib::OutputMode::ToMemory, /*instrument=*/true);
        ASSERT_TRUE(result.success) << result.diagnostics;
        // The allocation's call, whose column CodeView, the debug format of Windows targets,
        // does not record, and the stack object's declaration.
        EXPECT_NE(result.llvmIR.find("sub/dir/sites.c:8"), std::string::npos) << result.llvmIR;
        EXPECT_NE(result.llvmIR.find("sub/dir/sites.c:6\\00"), std::string::npos) << result.llvmIR;
    }

    // A unit compiled the way an analyser compiles it: unoptimised, with debug information.
    constexpr const char* kBitcodeUnit = R"(int shared;

static int twice(int value)
{
    return value * 2;
}

int main(void)
{
    shared = twice(21);
    return shared;
}
)";

    // In-memory bitcode is the file the same arguments write, byte for byte, and the file is
    // not written.
    void expectBitcodeMatchesFile(const std::vector<std::string>& args,
                                  const std::string& outputPath, bool instrument)
    {
        compilerlib::CompileResult file =
            compilerlib::compile(args, compilerlib::OutputMode::ToFile, instrument);
        ASSERT_TRUE(file.success) << file.diagnostics;
        const std::string written = readFile(outputPath);
        ASSERT_FALSE(written.empty());
        fs::remove(outputPath);

        compilerlib::CompileResult memory =
            compilerlib::compile(args, compilerlib::OutputMode::ToMemoryBitcode, instrument);
        ASSERT_TRUE(memory.success) << memory.diagnostics;
        EXPECT_EQ(memory.llvmBitcode, written);
        EXPECT_TRUE(memory.llvmIR.empty());
        EXPECT_FALSE(fs::exists(outputPath));
    }

    TEST_F(CompileTest, InMemoryBitcodeIsTheFileOutput)
    {
        expectBitcodeMatchesFile({"-O0", "-g", "-emit-llvm", "-c",
                                  writeSource("unit.c", kBitcodeUnit), "-o", path("unit.bc")},
                                 path("unit.bc"), /*instrument=*/false);
    }

    TEST_F(CompileTest, InstrumentedInMemoryBitcodeIsTheFileOutput)
    {
        expectBitcodeMatchesFile({"-O0", "-g", "-emit-llvm", "-c",
                                  writeSource("unit.c", kBitcodeUnit), "-o", path("unit.bc")},
                                 path("unit.bc"), /*instrument=*/true);
    }

#ifndef _WIN32
    // No temporary file stands in for the output: the temporary directory stays empty.
    TEST_F(CompileTest, InMemoryBitcodeCreatesNoTemporaryFile)
    {
        const std::string source = writeSource("unit.c", kBitcodeUnit);
        const fs::path temporary = path("tmp");
        fs::create_directories(temporary);
        const char* previous = std::getenv("TMPDIR");
        const std::string saved = previous != nullptr ? previous : "";
        setenv("TMPDIR", temporary.c_str(), 1);

        compilerlib::CompileResult memory =
            compilerlib::compile({"-O0", "-g", "-emit-llvm", "-c", source, "-o", path("unit.bc")},
                                 compilerlib::OutputMode::ToMemoryBitcode);

        if (previous != nullptr)
            setenv("TMPDIR", saved.c_str(), 1);
        else
            unsetenv("TMPDIR");
        ASSERT_TRUE(memory.success) << memory.diagnostics;
        EXPECT_TRUE(fs::is_empty(temporary));
    }
#endif

    // An output the caller asks for besides the bitcode is still written.
    TEST_F(CompileTest, InMemoryBitcodeKeepsTheRequestedDependencyFile)
    {
        compilerlib::CompileResult memory =
            compilerlib::compile({"-O0", "-emit-llvm", "-c", writeSource("unit.c", kBitcodeUnit),
                                  "-MD", "-MF", path("unit.d")},
                                 compilerlib::OutputMode::ToMemoryBitcode);
        ASSERT_TRUE(memory.success) << memory.diagnostics;
        EXPECT_FALSE(memory.llvmBitcode.empty());
        EXPECT_NE(readFile(path("unit.d")).find("unit.c"), std::string::npos);
    }

    // Only a bitcode compilation has bitcode to hold.
    TEST_F(CompileTest, InMemoryBitcodeNeedsABitcodeCompilation)
    {
        compilerlib::CompileResult memory =
            compilerlib::compile({"-S", "-emit-llvm", writeSource("unit.c", kBitcodeUnit)},
                                 compilerlib::OutputMode::ToMemoryBitcode);
        EXPECT_FALSE(memory.success);
        EXPECT_TRUE(memory.llvmBitcode.empty());
        EXPECT_NE(memory.diagnostics.find("-emit-llvm -c"), std::string::npos)
            << memory.diagnostics;
    }

    TEST_F(CompileTest, InMemoryBitcodeReportsCompileErrors)
    {
        compilerlib::CompileResult memory = compilerlib::compile(
            {"-emit-llvm", "-c", writeSource("broken.c", "int main(void) { return missing; }\n")},
            compilerlib::OutputMode::ToMemoryBitcode);
        EXPECT_FALSE(memory.success);
        EXPECT_TRUE(memory.llvmBitcode.empty());
        EXPECT_NE(memory.diagnostics.find("missing"), std::string::npos) << memory.diagnostics;
    }

    // Like clang, a failed compilation leaves no output behind: neither a partial object
    // nor an earlier one that would look up to date.
    class FailedCompilationTest : public CompileTest,
                                  public ::testing::WithParamInterface<const char*>
    {
    };

    TEST_P(FailedCompilationTest, LeavesNoOutput)
    {
        const std::string output = path("out.o");
        std::ofstream(output) << "previous";
        compilerlib::CompileResult result = compilerlib::compile(
            {"-c", std::string(CT_TEST_SOURCE_DIR "/examples/fixtures/") + GetParam(), "-o",
             output},
            compilerlib::OutputMode::ToFile, /*instrument=*/true);
        EXPECT_FALSE(result.success);
        EXPECT_FALSE(fs::exists(output)) << result.diagnostics;
    }

    // A code-generation error, and a frontend error.
    INSTANTIATE_TEST_SUITE_P(Errors, FailedCompilationTest,
                             ::testing::Values("codegen_error.c", "broken.c"),
                             [](const ::testing::TestParamInfo<const char*>& info)
                             {
                                 std::string name = info.param;
                                 return name.substr(0, name.find('.'));
                             });

#ifndef _WIN32
    // A write error, here the file size limit, fails compile(): left in the output stream,
    // it ended the process when the stream was destroyed.
    TEST_F(CompileTest, OutputWriteErrorFailsCompile)
    {
        struct rlimit saved;
        ASSERT_EQ(getrlimit(RLIMIT_FSIZE, &saved), 0);
        struct rlimit limited = saved;
        limited.rlim_cur = 512;
        // Without a handler, exceeding the limit raises SIGXFSZ instead of failing the write.
        auto previousHandler = std::signal(SIGXFSZ, SIG_IGN);
        ASSERT_EQ(setrlimit(RLIMIT_FSIZE, &limited), 0);
        compilerlib::CompileResult result = compilerlib::compile(
            {"-c", CT_TEST_SOURCE_DIR "/examples/fixtures/hello.c", "-o", path("hello.o")},
            compilerlib::OutputMode::ToFile, /*instrument=*/true);
        setrlimit(RLIMIT_FSIZE, &saved);
        std::signal(SIGXFSZ, previousHandler);

        EXPECT_FALSE(result.success);
        EXPECT_NE(result.diagnostics.find(std::strerror(EFBIG)), std::string::npos)
            << result.diagnostics;
        EXPECT_FALSE(fs::exists(path("hello.o")));
    }
#endif

} // namespace
