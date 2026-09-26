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
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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

} // namespace
