// SPDX-License-Identifier: Apache-2.0
#include "compilerlib/compiler.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TimeProfiler.h>
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
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
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

    // Functions that opt out of instrumentation, either way, and the ones beside them.
    constexpr const char* kNoInstrumentC =
        R"(__attribute__((no_instrument_function)) int quiet(int* values, int index)
{
    return values[index];
}

__attribute__((disable_sanitizer_instrumentation)) int quiet_too(int* values, int index)
{
    return values[index];
}

int loud(int* values, int index)
{
    return values[index];
}
)";

    constexpr const char* kNoInstrumentCxx = R"(struct Holder
{
    __attribute__((no_instrument_function)) int quiet(int* values, int index);
};

int Holder::quiet(int* values, int index)
{
    return values[index];
}

namespace
{
    __attribute__((no_instrument_function)) int quiet_internal(int* values, int index)
    {
        return values[index];
    }
} // namespace

int loud(int* values, int index)
{
    Holder holder;
    return holder.quiet(values, index) + quiet_internal(values, index) + values[index];
}
)";

    // For each function of `ir` defined with a name containing `part`: whether it calls into
    // the runtime.
    std::vector<bool> callsRuntime(const std::string& ir, const std::string& part)
    {
        llvm::LLVMContext context;
        llvm::SMDiagnostic error;
        std::unique_ptr<llvm::Module> module = llvm::parseAssemblyString(ir, error, context);
        std::vector<bool> result;
        if (!module)
        {
            ADD_FAILURE() << error.getMessage().str();
            return result;
        }
        for (const llvm::Function& func : *module)
        {
            if (func.isDeclaration() || func.getName().find(part) == llvm::StringRef::npos)
            {
                continue;
            }
            bool calls = false;
            for (const llvm::BasicBlock& block : func)
            {
                for (const llvm::Instruction& inst : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                    const llvm::Function* callee = call ? call->getCalledFunction() : nullptr;
                    calls |= callee && callee->getName().starts_with("__ct_");
                }
            }
            result.push_back(calls);
        }
        return result;
    }

    TEST_F(CompileTest, NoInstrumentFunctionIsLeftAlone)
    {
        compilerlib::CompileResult result =
            compilerlib::compile({"-S", "-emit-llvm", "--ct-modules=trace,alloc,bounds",
                                  writeSource("quiet.c", kNoInstrumentC)},
                                 compilerlib::OutputMode::ToMemory, /*instrument=*/true);
        ASSERT_TRUE(result.success) << result.diagnostics;
        EXPECT_EQ(callsRuntime(result.llvmIR, "quiet"), (std::vector<bool>{false, false}))
            << result.llvmIR;
        EXPECT_EQ(callsRuntime(result.llvmIR, "loud"), std::vector<bool>{true}) << result.llvmIR;
    }

    // A member function defined out of its class, whose definition inherits the attribute of
    // its declaration, and a function of internal linkage. Inline functions and templates
    // are never instrumented.
    TEST_F(CompileTest, NoInstrumentFunctionIsLeftAloneInCxx)
    {
        compilerlib::CompileResult result =
            compilerlib::compile({"-S", "-emit-llvm", "--ct-modules=trace,alloc,bounds",
                                  writeSource("quiet.cpp", kNoInstrumentCxx)},
                                 compilerlib::OutputMode::ToMemory, /*instrument=*/true);
        ASSERT_TRUE(result.success) << result.diagnostics;
        EXPECT_EQ(callsRuntime(result.llvmIR, "quiet"), (std::vector<bool>{false, false}))
            << result.llvmIR;
        EXPECT_EQ(callsRuntime(result.llvmIR, "loud"), std::vector<bool>{true}) << result.llvmIR;
    }

    // The functions that the definition of `name` in `ir` calls, by name, including those
    // called with another function type than they are declared with.
    std::vector<std::string> callees(const std::string& ir, const std::string& name)
    {
        llvm::LLVMContext context;
        llvm::SMDiagnostic error;
        std::unique_ptr<llvm::Module> module = llvm::parseAssemblyString(ir, error, context);
        std::vector<std::string> names;
        if (!module)
        {
            ADD_FAILURE() << error.getMessage().str();
            return names;
        }
        const llvm::Function* func = module->getFunction(name);
        if (!func || func->isDeclaration())
        {
            ADD_FAILURE() << "no definition of " << name;
            return names;
        }
        {
            for (const llvm::BasicBlock& block : *func)
            {
                for (const llvm::Instruction& inst : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                    const auto* callee = call ? llvm::dyn_cast<llvm::Function>(
                                                    call->getCalledOperand()->stripPointerCasts())
                                              : nullptr;
                    if (callee)
                    {
                        names.push_back(callee->getName().str());
                    }
                }
            }
        }
        return names;
    }

    // Whether `names` holds the runtime entry point `entry`, or its variant for a result the
    // program never uses.
    bool hasRuntimeEntry(const std::vector<std::string>& names, const std::string& entry)
    {
        return std::any_of(names.begin(), names.end(), [&](const std::string& name)
                           { return name == entry || name == entry + "_unreachable"; });
    }

    // setjmp returns twice, and LLVM keeps transformations such as tail calls away from its
    // callers only when the call carries returns_twice. A blanket -fno-builtin dropped it
    // from instrumented code (#136, and #153 for LLVM 16 and 17).
    constexpr const char* kSetjmp = R"(#include <setjmp.h>

static jmp_buf recover;
int use(int value);

int jump(int value)
{
    if (setjmp(recover))
    {
        return use(value);
    }
    return use(value + 1);
}
)";

    TEST_F(CompileTest, SetjmpReturnsTwiceInInstrumentedCode)
    {
        compilerlib::CompileResult result =
            compilerlib::compile({"-O2", "-S", "-emit-llvm", writeSource("jump.c", kSetjmp)},
                                 compilerlib::OutputMode::ToMemory, /*instrument=*/true);
        ASSERT_TRUE(result.success) << result.diagnostics;

        llvm::LLVMContext context;
        llvm::SMDiagnostic error;
        std::unique_ptr<llvm::Module> module =
            llvm::parseAssemblyString(result.llvmIR, error, context);
        ASSERT_NE(module, nullptr) << error.getMessage().str();
        int setjmpCalls = 0;
        for (const llvm::Function& func : *module)
        {
            for (const llvm::BasicBlock& block : func)
            {
                for (const llvm::Instruction& inst : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                    const llvm::Function* callee = call ? call->getCalledFunction() : nullptr;
                    if (!callee || callee->getName().find("setjmp") == llvm::StringRef::npos)
                    {
                        continue;
                    }
                    ++setjmpCalls;
                    EXPECT_TRUE(call->hasFnAttr(llvm::Attribute::ReturnsTwice)) << result.llvmIR;
                    EXPECT_FALSE(call->isTailCall()) << result.llvmIR;
                }
            }
        }
        EXPECT_EQ(setjmpCalls, 1) << result.llvmIR;
    }

    // At -O2, clang may remove an allocation released in the same function, unless the
    // allocation function is not a builtin for it, or, for operator new, the call is not
    // marked as one it may omit. Each pair here would be removed: the pass must still see
    // and track every allocation.
    constexpr const char* kElidableCAllocations = R"(typedef __SIZE_TYPE__ size_t;
void* malloc(size_t size);
void* calloc(size_t count, size_t size);
void* realloc(void* block, size_t size);
void* aligned_alloc(size_t alignment, size_t size);
int posix_memalign(void** block, size_t alignment, size_t size);
void free(void* block);

int viaMalloc(void)
{
    char* block = malloc(4);
    block[0] = 1;
    int value = block[0];
    free(block);
    return value;
}

int viaCalloc(void)
{
    char* block = calloc(4, 1);
    int value = block[1];
    free(block);
    return value;
}

int viaRealloc(void)
{
    char* block = realloc(0, 4);
    block[0] = 2;
    int value = block[0];
    free(block);
    return value;
}

int viaAlignedAlloc(void)
{
    char* block = aligned_alloc(16, 16);
    block[0] = 3;
    int value = block[0];
    free(block);
    return value;
}

int viaPosixMemalign(void)
{
    void* block = 0;
    if (posix_memalign(&block, 16, 16) != 0)
    {
        return 0;
    }
    ((char*)block)[0] = 4;
    int value = ((char*)block)[0];
    free(block);
    return value;
}
)";

    TEST_F(CompileTest, ElidableCAllocationsStayTrackedAtO2)
    {
        compilerlib::CompileResult result =
            compilerlib::compile({"-O2", "-S", "-emit-llvm", "--ct-modules=alloc",
                                  writeSource("elidable.c", kElidableCAllocations)},
                                 compilerlib::OutputMode::ToMemory, /*instrument=*/true);
        ASSERT_TRUE(result.success) << result.diagnostics;
        EXPECT_TRUE(hasRuntimeEntry(callees(result.llvmIR, "viaMalloc"), "__ct_malloc"))
            << result.llvmIR;
        EXPECT_TRUE(hasRuntimeEntry(callees(result.llvmIR, "viaCalloc"), "__ct_calloc"))
            << result.llvmIR;
        EXPECT_TRUE(hasRuntimeEntry(callees(result.llvmIR, "viaRealloc"), "__ct_realloc"))
            << result.llvmIR;
        EXPECT_TRUE(
            hasRuntimeEntry(callees(result.llvmIR, "viaAlignedAlloc"), "__ct_aligned_alloc"))
            << result.llvmIR;
        EXPECT_TRUE(
            hasRuntimeEntry(callees(result.llvmIR, "viaPosixMemalign"), "__ct_posix_memalign"))
            << result.llvmIR;
    }

    // The functions are extern "C", to have one name on the Itanium and Microsoft C++ ABIs.
    constexpr const char* kElidableCxxAllocations = R"(namespace std
{
    struct nothrow_t
    {
    };
    extern const nothrow_t nothrow;
} // namespace std

void* operator new(__SIZE_TYPE__ size, const std::nothrow_t&) noexcept;
void* operator new[](__SIZE_TYPE__ size, const std::nothrow_t&) noexcept;

extern "C" int viaNew()
{
    int* block = new int(1);
    int value = *block;
    delete block;
    return value;
}

extern "C" int viaNewArray()
{
    int* block = new int[4]();
    int value = block[1];
    delete[] block;
    return value;
}

extern "C" int viaNothrowNew()
{
    int* block = new (std::nothrow) int(2);
    int value = block ? *block : 0;
    delete block;
    return value;
}

extern "C" int viaNothrowNewArray()
{
    int* block = new (std::nothrow) int[4]();
    int value = block ? block[1] : 0;
    delete[] block;
    return value;
}

// Aligned operator new is not tracked: the runtime allocates through the unaligned one.
// It is called explicitly: whether a new-expression uses it depends on the target.
namespace std
{
    enum class align_val_t : __SIZE_TYPE__
    {
    };
} // namespace std

void* operator new(__SIZE_TYPE__ size, std::align_val_t alignment);
void operator delete(void* block, std::align_val_t alignment) noexcept;

extern "C" int viaAlignedNew()
{
    char* block = static_cast<char*>(::operator new(64, std::align_val_t(64)));
    block[0] = 5;
    int value = block[0];
    ::operator delete(block, std::align_val_t(64));
    return value;
}
)";

    TEST_F(CompileTest, ElidableCxxAllocationsStayTrackedAtO2)
    {
        compilerlib::CompileResult result =
            compilerlib::compile({"-O2", "-S", "-emit-llvm", "--ct-modules=alloc",
                                  writeSource("elidable.cpp", kElidableCxxAllocations)},
                                 compilerlib::OutputMode::ToMemory, /*instrument=*/true);
        ASSERT_TRUE(result.success) << result.diagnostics;
        EXPECT_TRUE(hasRuntimeEntry(callees(result.llvmIR, "viaNew"), "__ct_new")) << result.llvmIR;
        EXPECT_TRUE(hasRuntimeEntry(callees(result.llvmIR, "viaNewArray"), "__ct_new_array"))
            << result.llvmIR;
        EXPECT_TRUE(hasRuntimeEntry(callees(result.llvmIR, "viaNothrowNew"), "__ct_new_nothrow"))
            << result.llvmIR;
        EXPECT_TRUE(
            hasRuntimeEntry(callees(result.llvmIR, "viaNothrowNewArray"), "__ct_new_array_nothrow"))
            << result.llvmIR;
        const std::vector<std::string> alignedCallees = callees(result.llvmIR, "viaAlignedNew");
        EXPECT_FALSE(alignedCallees.empty()) << result.llvmIR;
        for (const std::string& callee : alignedCallees)
        {
            EXPECT_FALSE(llvm::StringRef(callee).starts_with("__ct_new")) << result.llvmIR;
        }
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

#ifdef __APPLE__
    // Compiles once while the sysroot detection cannot work, then once it can: the second
    // compilation must find the system headers. Exits with 0 when it does.
    [[noreturn]] void compileAfterAFailedSysrootDetection(const std::string& headerFree,
                                                          const std::string& withHeader)
    {
        const char* previous = std::getenv("TMPDIR");
        const std::string saved = previous != nullptr ? previous : "";
        // The detection runs xcrun through temporary files, which cannot be created here; the
        // source needs no system header, so this compilation succeeds without a sysroot.
        setenv("TMPDIR", "/dev/null", 1);
        const compilerlib::CompileResult first = compilerlib::compile(
            {"-S", "-emit-llvm", headerFree}, compilerlib::OutputMode::ToMemory);
        if (previous != nullptr)
            setenv("TMPDIR", saved.c_str(), 1);
        else
            unsetenv("TMPDIR");

        const compilerlib::CompileResult second = compilerlib::compile(
            {"-S", "-emit-llvm", withHeader}, compilerlib::OutputMode::ToMemory);
        std::exit(first.success && second.success ? 0 : 1);
    }

    // A failed sysroot detection is not kept for the process: once its cause is gone, the next
    // compilation detects the sysroot again and finds the system headers (#98). Runs in a fresh
    // process, since an earlier test of this binary has already detected the sysroot, and a
    // detection that succeeded is rightly kept, which would leave no failure to recover from.
    TEST_F(CompileTest, FailedSysrootDetectionIsRetried)
    {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
        const std::string headerFree = writeSource("plain.c", "int main(void) { return 0; }\n");
        const std::string withHeader = writeSource(
            "hello.c", "#include <stdio.h>\nint main(void) { return puts(\"hi\") < 0; }\n");
        EXPECT_EXIT(compileAfterAFailedSysrootDetection(headerFree, withHeader),
                    ::testing::ExitedWithCode(0), "");
    }
#endif

    // Sets an environment variable for its lifetime, then restores the previous value.
    class ScopedEnv
    {
      public:
        ScopedEnv(const char* name, const std::string& value) : name_(name)
        {
            if (const char* previous = std::getenv(name))
                previous_ = previous;
            set(value.c_str());
        }

        ~ScopedEnv()
        {
            if (previous_)
                set(previous_->c_str());
            else
                unset();
        }

        ScopedEnv(const ScopedEnv&) = delete;
        ScopedEnv& operator=(const ScopedEnv&) = delete;

      private:
        void set(const char* value) const
        {
#ifdef _WIN32
            (void)_putenv_s(name_, value);
#else
            (void)setenv(name_, value, 1);
#endif
        }

        void unset() const
        {
#ifdef _WIN32
            (void)_putenv_s(name_, "");
#else
            (void)unsetenv(name_);
#endif
        }

        const char* name_;
        std::optional<std::string> previous_;
    };

    // As in a relocated install that ships Clang's headers and no clang (#104): CT_CLANG
    // names a file that exists but cannot run. File output without instrumentation must
    // still compile, in this process, like every other mode.
    class NoClangExecutableTest : public CompileTest
    {
      protected:
        void SetUp() override
        {
            CompileTest::SetUp();
            clang_.emplace("CT_CLANG", writeSource("bin/clang", "not an executable\n"));
        }

        void TearDown() override
        {
            clang_.reset();
            CompileTest::TearDown();
        }

      private:
        std::optional<ScopedEnv> clang_;
    };

    constexpr const char* kUnit = "int twice(int value) { return value * 2; }\n";

    struct FileOutputCase
    {
        const char* name;
        std::vector<std::string> args;
        const char* output;
    };

    class FileOutputTest : public NoClangExecutableTest,
                           public ::testing::WithParamInterface<FileOutputCase>
    {
    };

    TEST_P(FileOutputTest, IsWrittenWithoutAClangExecutable)
    {
        std::vector<std::string> args = GetParam().args;
        args.push_back(writeSource("unit.c", kUnit));
        args.push_back("-o");
        args.push_back(path(GetParam().output));

        compilerlib::CompileResult result = compilerlib::compile(args);
        ASSERT_TRUE(result.success) << result.diagnostics;
        EXPECT_FALSE(readFile(path(GetParam().output)).empty());
    }

    INSTANTIATE_TEST_SUITE_P(
        Outputs, FileOutputTest,
        ::testing::Values(FileOutputCase{"bitcode", {"-c", "-emit-llvm"}, "unit.bc"},
                          FileOutputCase{"ir", {"-S", "-emit-llvm"}, "unit.ll"},
                          FileOutputCase{"assembly", {"-S"}, "unit.s"},
                          FileOutputCase{"object", {"-c"}, "unit.o"},
                          FileOutputCase{"preprocessed", {"-E"}, "unit.i"}),
        [](const ::testing::TestParamInfo<FileOutputCase>& info) { return info.param.name; });

    TEST_F(NoClangExecutableTest, SyntaxOnlySucceeds)
    {
        compilerlib::CompileResult result =
            compilerlib::compile({"-fsyntax-only", writeSource("unit.c", kUnit)});
        EXPECT_TRUE(result.success) << result.diagnostics;
    }

    // The compiler's own diagnostic reaches the caller, not a bare "compilation failed".
    TEST_F(NoClangExecutableTest, InvalidSourceReportsTheCompilerDiagnostic)
    {
        compilerlib::CompileResult result = compilerlib::compile(
            {"-c", writeSource("broken.c", "int main(void) { return undeclared; }\n"), "-o",
             path("broken.o")});
        ASSERT_FALSE(result.success);
        EXPECT_NE(result.diagnostics.find("broken.c:1:"), std::string::npos) << result.diagnostics;
        EXPECT_NE(result.diagnostics.find("use of undeclared identifier 'undeclared'"),
                  std::string::npos)
            << result.diagnostics;
        EXPECT_FALSE(fs::exists(path("broken.o")));
    }

    // Several sources in one call, compiled and linked: the driver links with the system
    // linker, not with clang.
    TEST_F(NoClangExecutableTest, SeveralSourcesAreCompiledAndLinked)
    {
        compilerlib::CompileResult result = compilerlib::compile(
            {writeSource("main.c", "int twice(int value);\nint main(void) { return twice(0); }\n"),
             writeSource("unit.c", kUnit), "-o", path("app")});
        ASSERT_TRUE(result.success) << result.diagnostics;
        EXPECT_TRUE(fs::exists(path("app")) || fs::exists(path("app.exe")));
    }

    // Actions beyond code generation keep working: the static analyzer and precompiled
    // headers.
    TEST_F(NoClangExecutableTest, AnalyzerReportsItsFinding)
    {
        compilerlib::CompileResult result = compilerlib::compile(
            {"--analyze", writeSource("null.c", "int main(void) { int* p = 0; return *p; }\n"),
             "-o", path("null.plist")});
        ASSERT_TRUE(result.success) << result.diagnostics;
        EXPECT_NE(result.diagnostics.find("Dereference of null pointer"), std::string::npos)
            << result.diagnostics;
        EXPECT_TRUE(fs::exists(path("null.plist")));
    }

    TEST_F(NoClangExecutableTest, PrecompiledHeaderIsWrittenAndUsable)
    {
        compilerlib::CompileResult header = compilerlib::compile(
            {"-x", "c-header", writeSource("unit.h", "int twice(int value);\n"), "-o",
             path("unit.h.pch")});
        ASSERT_TRUE(header.success) << header.diagnostics;

        compilerlib::CompileResult user =
            compilerlib::compile({"-include-pch", path("unit.h.pch"), "-fsyntax-only",
                                  writeSource("user.c", "int four(void) { return twice(2); }\n")});
        EXPECT_TRUE(user.success) << user.diagnostics;
    }

    // -fno-integrated-cc1 asks, as with clang, for the frontend in a clang process: here,
    // CT_CLANG cannot run, so the compilation fails for that reason.
    TEST_F(NoClangExecutableTest, NoIntegratedCc1RunsTheClangExecutable)
    {
        compilerlib::CompileResult result = compilerlib::compile(
            {"-fno-integrated-cc1", "-c", writeSource("unit.c", kUnit), "-o", path("unit.o")});
        EXPECT_FALSE(result.success);
        EXPECT_NE(result.diagnostics.find("unable to execute"), std::string::npos)
            << result.diagnostics;
    }

    // -### prints the jobs and runs none of them.
    TEST_F(NoClangExecutableTest, HashHashHashRunsNothing)
    {
        compilerlib::CompileResult result = compilerlib::compile(
            {"-###", "-c", writeSource("unit.c", kUnit), "-o", path("unit.o")});
        EXPECT_TRUE(result.success) << result.diagnostics;
        EXPECT_FALSE(fs::exists(path("unit.o")));
    }

    // Instrumented objects are emitted by Clang's backend with the invocation's code
    // generation options, as plain ones are (#131). The sources have no headers, so that an
    // explicit --target checks ELF and COFF objects from any host.
    constexpr const char* kSectionUnit = R"(int counter;
int ratio = 3;

int first(int value)
{
    return value + counter;
}

int second(int value)
{
    return value * ratio;
}
)";

    constexpr const char* kElfTarget = "--target=x86_64-unknown-linux-gnu";
    constexpr const char* kCoffTarget = "--target=x86_64-pc-windows-msvc";

    using ObjectBinary = llvm::object::OwningBinary<llvm::object::ObjectFile>;

    // The object file at `path`, or an empty binary after a test failure.
    ObjectBinary openObject(const std::string& path)
    {
        llvm::Expected<ObjectBinary> binary = llvm::object::ObjectFile::createObjectFile(path);
        if (!binary)
        {
            ADD_FAILURE() << path << ": " << llvm::toString(binary.takeError());
            return {};
        }
        return std::move(*binary);
    }

    // The section of each symbol of `names` the object defines, by section index.
    std::map<std::string, uint64_t> sectionsOf(const llvm::object::ObjectFile& object,
                                               const std::vector<std::string>& names)
    {
        std::map<std::string, uint64_t> sections;
        for (const llvm::object::SymbolRef& symbol : object.symbols())
        {
            llvm::Expected<llvm::StringRef> name = symbol.getName();
            llvm::Expected<llvm::object::section_iterator> section = symbol.getSection();
            if (!name || !section)
            {
                llvm::consumeError(name.takeError());
                llvm::consumeError(section.takeError());
                continue;
            }
            if (*section != object.section_end() &&
                std::find(names.begin(), names.end(), name->str()) != names.end())
            {
                sections[name->str()] = (*section)->getIndex();
            }
        }
        return sections;
    }

    bool hasSection(const llvm::object::ObjectFile& object, llvm::StringRef wanted)
    {
        for (const llvm::object::SectionRef& section : object.sections())
        {
            llvm::Expected<llvm::StringRef> name = section.getName();
            if (name && *name == wanted)
                return true;
            if (!name)
                llvm::consumeError(name.takeError());
        }
        return false;
    }

    class EmissionOptionsTest : public CompileTest,
                                public ::testing::WithParamInterface<const char*>
    {
      protected:
        // The object of kSectionUnit for the test's target, plain or instrumented.
        ObjectBinary compileUnit(const char* name, bool instrument, std::vector<std::string> extra)
        {
            std::vector<std::string> args{
                GetParam(), "-O2", "-c", writeSource("unit.c", kSectionUnit), "-o", path(name)};
            args.insert(args.end(), extra.begin(), extra.end());
            if (instrument)
                args.push_back("--ct-modules=alloc");
            compilerlib::CompileResult result =
                compilerlib::compile(args, compilerlib::OutputMode::ToFile, instrument);
            EXPECT_TRUE(result.success) << result.diagnostics;
            return result.success ? openObject(path(name)) : ObjectBinary();
        }
    };

    // Each function and each global of the source in a section of its own. The sections
    // themselves are not compared with the plain object's: an instrumented object has more,
    // such as the runtime configuration's.
    TEST_P(EmissionOptionsTest, FunctionAndDataSectionsAreKept)
    {
        const std::vector<std::string> names{"first", "second", "counter", "ratio"};
        for (const bool instrument : {false, true})
        {
            ObjectBinary object = compileUnit(instrument ? "instrumented.o" : "plain.o", instrument,
                                              {"-ffunction-sections", "-fdata-sections"});
            ASSERT_NE(object.getBinary(), nullptr);
            const std::map<std::string, uint64_t> sections = sectionsOf(*object.getBinary(), names);
            ASSERT_EQ(sections.size(), names.size()) << (instrument ? "instrumented" : "plain");
            std::map<uint64_t, std::string> owners;
            for (const auto& [name, index] : sections)
            {
                const auto [owner, added] = owners.emplace(index, name);
                EXPECT_TRUE(added) << (instrument ? "instrumented" : "plain") << " object: " << name
                                   << " shares its section with " << owner->second;
            }
        }
    }

    // The address-significance table that lets the linker fold identical code.
    TEST_P(EmissionOptionsTest, AddressSignificanceTableIsKept)
    {
        ObjectBinary plain = compileUnit("plain.o", false, {});
        ObjectBinary instrumented = compileUnit("instrumented.o", true, {});
        ASSERT_NE(plain.getBinary(), nullptr);
        ASSERT_NE(instrumented.getBinary(), nullptr);
        ASSERT_TRUE(hasSection(*plain.getBinary(), ".llvm_addrsig")) << "the test's premise";
        EXPECT_TRUE(hasSection(*instrumented.getBinary(), ".llvm_addrsig"));
    }

    INSTANTIATE_TEST_SUITE_P(ObjectFormats, EmissionOptionsTest,
                             ::testing::Values(kElfTarget, kCoffTarget),
                             [](const ::testing::TestParamInfo<const char*>& info)
                             { return info.param == kElfTarget ? "Elf" : "Coff"; });

    // The files code generation writes next to the object: split debug information
    // (-gsplit-dwarf) and stack usage (-fstack-usage).
    TEST_F(CompileTest, InstrumentedObjectWritesSecondaryOutputs)
    {
        compilerlib::CompileResult result = compilerlib::compile(
            {kElfTarget, "-O2", "-g", "-gsplit-dwarf", "-fstack-usage", "-c",
             writeSource("unit.c", kSectionUnit), "-o", path("unit.o"), "--ct-modules=alloc"},
            compilerlib::OutputMode::ToFile, /*instrument=*/true);
        ASSERT_TRUE(result.success) << result.diagnostics;
        EXPECT_TRUE(fs::exists(path("unit.o")));
        EXPECT_TRUE(fs::exists(path("unit.dwo")));
        EXPECT_TRUE(fs::exists(path("unit.su")));
    }

    TEST_F(CompileTest, FailedInstrumentedObjectLeavesNoSecondaryOutput)
    {
        compilerlib::CompileResult result = compilerlib::compile(
            {kElfTarget, "-g", "-gsplit-dwarf", "-fstack-usage", "-c",
             CT_TEST_SOURCE_DIR "/examples/fixtures/codegen_error.c", "-o", path("broken.o")},
            compilerlib::OutputMode::ToFile, /*instrument=*/true);
        ASSERT_FALSE(result.success);
        EXPECT_FALSE(fs::exists(path("broken.o")));
        EXPECT_FALSE(fs::exists(path("broken.dwo")));
        EXPECT_FALSE(fs::exists(path("broken.su")));
    }

    // How many times each optimization pass of `passes` runs, from -fdebug-pass-manager.
    std::map<std::string, int> countPasses(const std::string& log,
                                           const std::vector<std::string>& passes)
    {
        std::map<std::string, int> counts;
        for (const std::string& pass : passes)
        {
            const std::string needle = "Running pass: " + pass + " ";
            for (size_t at = log.find(needle); at != std::string::npos;
                 at = log.find(needle, at + 1))
            {
                ++counts[pass];
            }
        }
        return counts;
    }

    // Instrumented code is optimized once, as plain code is: no step of the instrumented
    // compilation runs the optimization pipeline again. The instrumentation passes are not
    // pass-manager passes and print nothing here.
    TEST_F(CompileTest, InstrumentedCompilationOptimizesOnce)
    {
        const std::string source = writeSource("unit.c", kSectionUnit);
        const std::vector<std::string> passes{"SROAPass", "InstCombinePass", "GVNPass",
                                              "SimplifyCFGPass"};
        std::map<std::string, int> counts[2];
        for (const bool instrument : {false, true})
        {
            ::testing::internal::CaptureStderr();
            compilerlib::CompileResult result = compilerlib::compile(
                {kElfTarget, "-O2", "-Xclang", "-fdebug-pass-manager", "-c", source, "-o",
                 path(instrument ? "instrumented.o" : "plain.o"), "--ct-modules=alloc"},
                compilerlib::OutputMode::ToFile, instrument);
            const std::string log = ::testing::internal::GetCapturedStderr();
            ASSERT_TRUE(result.success) << result.diagnostics;
            counts[instrument] = countPasses(log + result.diagnostics, passes);
        }
        for (const std::string& pass : passes)
        {
            EXPECT_GT(counts[false][pass], 0) << pass << ": the test's premise";
            EXPECT_EQ(counts[true][pass], counts[false][pass]) << pass;
        }
    }

    // The instrumented IR and the instrumented object come from the same module, with the
    // same options: compiling the IR without optimization gives the object's code. This
    // checks option parity, not the absence of a second optimization.
    TEST_F(CompileTest, InstrumentedObjectIsTheCodeOfTheInstrumentedIR)
    {
        const std::string source = writeSource("unit.c", kSectionUnit);
        const std::vector<std::string> common{kElfTarget, "-O2", "-fPIE", "-ffunction-sections"};

        std::vector<std::string> irArgs = common;
        irArgs.insert(irArgs.end(), {"-S", "-emit-llvm", source, "--ct-modules=alloc"});
        compilerlib::CompileResult ir =
            compilerlib::compile(irArgs, compilerlib::OutputMode::ToMemory, /*instrument=*/true);
        ASSERT_TRUE(ir.success) << ir.diagnostics;
        std::ofstream(path("instrumented.ll")) << ir.llvmIR;

        std::vector<std::string> fromIrArgs = common;
        fromIrArgs.insert(fromIrArgs.end(), {"-Xclang", "-disable-llvm-passes", "-c",
                                             path("instrumented.ll"), "-o", path("from_ir.o")});
        compilerlib::CompileResult fromIr =
            compilerlib::compile(fromIrArgs, compilerlib::OutputMode::ToFile);
        ASSERT_TRUE(fromIr.success) << fromIr.diagnostics;

        std::vector<std::string> objectArgs = common;
        objectArgs.insert(objectArgs.end(),
                          {"-c", source, "-o", path("instrumented.o"), "--ct-modules=alloc"});
        compilerlib::CompileResult object =
            compilerlib::compile(objectArgs, compilerlib::OutputMode::ToFile, /*instrument=*/true);
        ASSERT_TRUE(object.success) << object.diagnostics;

        auto codeSections = [](const llvm::object::ObjectFile& file)
        {
            std::map<std::string, std::string> code;
            for (const llvm::object::SectionRef& section : file.sections())
            {
                llvm::Expected<llvm::StringRef> name = section.getName();
                llvm::Expected<llvm::StringRef> contents = section.getContents();
                if (name && contents && section.isText())
                    code[name->str()] = contents->str();
                if (!name)
                    llvm::consumeError(name.takeError());
                if (!contents)
                    llvm::consumeError(contents.takeError());
            }
            return code;
        };
        ObjectBinary fromIrObject = openObject(path("from_ir.o"));
        ObjectBinary instrumentedObject = openObject(path("instrumented.o"));
        ASSERT_NE(fromIrObject.getBinary(), nullptr);
        ASSERT_NE(instrumentedObject.getBinary(), nullptr);
        EXPECT_EQ(codeSections(*instrumentedObject.getBinary()),
                  codeSections(*fromIrObject.getBinary()));
    }

    // Explicit calls to the memory functions become memory intrinsics, which the bounds
    // module checks, once clang may treat these functions as builtins (#153).
    constexpr const char* kMemoryFunctions = R"(typedef __SIZE_TYPE__ size_t;
void* memcpy(void* destination, const void* source, size_t size);
void* memset(void* destination, int value, size_t size);
void* memmove(void* destination, const void* source, size_t size);

void copyBytes(char* destination, const char* source, size_t size)
{
    memcpy(destination, source, size);
}

void fillBytes(char* destination, size_t size)
{
    memset(destination, 0, size);
}

void moveBytes(char* destination, const char* source, size_t size)
{
    memmove(destination, source, size);
}
)";

    TEST_F(CompileTest, MemoryFunctionsAreBoundsChecked)
    {
        compilerlib::CompileResult result =
            compilerlib::compile({"-O2", "-S", "-emit-llvm", "--ct-modules=bounds",
                                  writeSource("memory.c", kMemoryFunctions)},
                                 compilerlib::OutputMode::ToMemory, /*instrument=*/true);
        ASSERT_TRUE(result.success) << result.diagnostics;
        for (const char* function : {"copyBytes", "fillBytes", "moveBytes"})
        {
            const std::vector<std::string> called = callees(result.llvmIR, function);
            EXPECT_NE(std::find(called.begin(), called.end(), "__ct_check_bounds"), called.end())
                << function << " is not bounds-checked\n"
                << result.llvmIR;
        }
    }

    // The remarks an optimization record holds, as (kind, pass, name, function), ignoring
    // their arguments, locations and order.
    std::multiset<std::string> recordedRemarks(const std::string& path)
    {
        std::multiset<std::string> remarks;
        std::ifstream in(path);
        std::string line;
        std::string kind;
        std::string pass;
        std::string name;
        auto value = [](const std::string& field)
        {
            const size_t colon = field.find(':');
            const size_t start = field.find_first_not_of(' ', colon + 1);
            return start == std::string::npos ? std::string() : field.substr(start);
        };
        while (std::getline(in, line))
        {
            if (line.rfind("--- ", 0) == 0)
                kind = line.substr(4);
            else if (line.rfind("Pass:", 0) == 0)
                pass = value(line);
            else if (line.rfind("Name:", 0) == 0)
                name = value(line);
            else if (line.rfind("Function:", 0) == 0)
                remarks.insert(kind + " " + pass + " " + name + " " + value(line));
        }
        return remarks;
    }

    // -fsave-optimization-record records the remarks of optimization and of code
    // generation, for instrumented code as for plain code. The instrumentation passes record
    // none, and define no function.
    TEST_F(CompileTest, InstrumentedOptimizationRecordMatchesPlain)
    {
        const std::string source = writeSource("unit.c", kSectionUnit);
        std::multiset<std::string> remarks[2];
        for (const bool instrument : {false, true})
        {
            const std::string object = path(instrument ? "instrumented.o" : "plain.o");
            compilerlib::CompileResult result =
                compilerlib::compile({kElfTarget, "-O2", "-fsave-optimization-record", "-c", source,
                                      "-o", object, "--ct-modules=alloc"},
                                     compilerlib::OutputMode::ToFile, instrument);
            ASSERT_TRUE(result.success) << result.diagnostics;
            remarks[instrument] =
                recordedRemarks(path(instrument ? "instrumented.opt.yaml" : "plain.opt.yaml"));
        }
        EXPECT_FALSE(remarks[false].empty()) << "the test's premise";
        EXPECT_EQ(remarks[true], remarks[false]);
    }

    TEST_F(CompileTest, FailedInstrumentedCompilationLeavesNoOptimizationRecord)
    {
        compilerlib::CompileResult result = compilerlib::compile(
            {kElfTarget, "-O2", "-fsave-optimization-record", "-c",
             CT_TEST_SOURCE_DIR "/examples/fixtures/codegen_error.c", "-o", path("broken.o")},
            compilerlib::OutputMode::ToFile, /*instrument=*/true);
        ASSERT_FALSE(result.success);
        EXPECT_FALSE(fs::exists(path("broken.opt.yaml")));
    }

    // Instrumented LTO is not supported (#163): the bitcode would be written without its
    // summary, and the link-time pipeline would optimize the instrumented code again. The
    // compilation fails at once, and writes nothing.
    class InstrumentedLtoTest : public CompileTest,
                                public ::testing::WithParamInterface<const char*>
    {
    };

    TEST_P(InstrumentedLtoTest, IsRejected)
    {
        compilerlib::CompileResult result =
            compilerlib::compile({kElfTarget, GetParam(), "-c", writeSource("unit.c", kSectionUnit),
                                  "-o", path("unit.o"), "--ct-modules=alloc"},
                                 compilerlib::OutputMode::ToFile, /*instrument=*/true);
        EXPECT_FALSE(result.success);
        EXPECT_NE(result.diagnostics.find("--instrument does not support -flto"), std::string::npos)
            << result.diagnostics;
        EXPECT_FALSE(fs::exists(path("unit.o")));
    }

    INSTANTIATE_TEST_SUITE_P(LtoModes, InstrumentedLtoTest,
                             ::testing::Values("-flto", "-flto=thin", "-flto=full"),
                             [](const ::testing::TestParamInfo<const char*>& info)
                             {
                                 const std::string mode = info.param;
                                 return mode == "-flto" ? std::string("Default")
                                                        : mode.substr(std::strlen("-flto="));
                             });

    // -fno-lto after -flto turns LTO off, as for clang.
    TEST_F(CompileTest, InstrumentedCompilationAcceptsLtoTurnedOff)
    {
        compilerlib::CompileResult result = compilerlib::compile(
            {kElfTarget, "-flto", "-fno-lto", "-c", writeSource("unit.c", kSectionUnit), "-o",
             path("unit.o"), "--ct-modules=alloc"},
            compilerlib::OutputMode::ToFile, /*instrument=*/true);
        EXPECT_TRUE(result.success) << result.diagnostics;
        EXPECT_TRUE(fs::exists(path("unit.o")));
    }

    // Plain compilations keep LTO.
    TEST_F(CompileTest, PlainLtoIsUnchanged)
    {
        compilerlib::CompileResult result =
            compilerlib::compile({kElfTarget, "-flto=thin", "-c",
                                  writeSource("unit.c", kSectionUnit), "-o", path("unit.o")},
                                 compilerlib::OutputMode::ToFile);
        EXPECT_TRUE(result.success) << result.diagnostics;
        EXPECT_TRUE(fs::exists(path("unit.o")));
    }
    // The names of the events of a -ftime-trace file; empty if the file is missing or is
    // not a trace.
    std::set<std::string> traceEvents(const std::string& path)
    {
        std::set<std::string> names;
        llvm::Expected<llvm::json::Value> trace = llvm::json::parse(readFile(path));
        if (!trace)
        {
            llvm::consumeError(trace.takeError());
            return names;
        }
        const llvm::json::Object* object = trace->getAsObject();
        const llvm::json::Array* events = object ? object->getArray("traceEvents") : nullptr;
        if (!events)
            return names;
        for (const llvm::json::Value& event : *events)
        {
            if (const llvm::json::Object* fields = event.getAsObject())
            {
                if (auto name = fields->getString("name"))
                    names.insert(name->str());
            }
        }
        return names;
    }

    // -ftime-trace writes the trace clang writes, on every path a compilation to a file
    // takes (#166), and the profiler stops with the compilation.
    struct TimeTraceMode
    {
        const char* name;
        bool instrument;
        const char* option;
    };

    void PrintTo(const TimeTraceMode& mode, std::ostream* os)
    {
        *os << mode.name;
    }

    class TimeTraceTest : public CompileTest, public ::testing::WithParamInterface<TimeTraceMode>
    {
      protected:
        compilerlib::CompileResult compileWithTrace(const std::string& traceOption)
        {
            // Granularity 0 records every event, however short the compilation.
            std::vector<std::string> args = {kElfTarget, "-O2", traceOption,
                                             "-ftime-trace-granularity=0"};
            args.insert(args.end(),
                        {"-c", writeSource("unit.c", kSectionUnit), "-o", path("unit.o")});
            if (GetParam().option)
                args.push_back(GetParam().option);
            return compilerlib::compile(args, compilerlib::OutputMode::ToFile,
                                        GetParam().instrument);
        }
    };

    TEST_P(TimeTraceTest, WritesTheTraceNextToTheOutput)
    {
        compilerlib::CompileResult result = compileWithTrace("-ftime-trace");
        ASSERT_TRUE(result.success) << result.diagnostics;
        const std::set<std::string> events = traceEvents(path("unit.json"));
        for (const char* step : {"Frontend", "Optimizer", "CodeGenPasses"})
            EXPECT_EQ(events.count(step), 1u) << step;
        EXPECT_FALSE(llvm::timeTraceProfilerEnabled());
    }

    TEST_P(TimeTraceTest, WritesTheTraceWhereAsked)
    {
        compilerlib::CompileResult result = compileWithTrace("-ftime-trace=" + path("custom.json"));
        ASSERT_TRUE(result.success) << result.diagnostics;
        EXPECT_EQ(traceEvents(path("custom.json")).count("Frontend"), 1u);
        EXPECT_FALSE(fs::exists(path("unit.json")));
    }

    // As with clang, a trace that cannot be written is reported, and the compilation still
    // succeeds.
    TEST_P(TimeTraceTest, ReportsAnUnwritableTrace)
    {
        compilerlib::CompileResult result =
            compileWithTrace("-ftime-trace=" + path("missing/trace.json"));
        EXPECT_TRUE(result.success) << result.diagnostics;
        EXPECT_NE(result.diagnostics.find("unable to open output file"), std::string::npos)
            << result.diagnostics;
        EXPECT_TRUE(fs::exists(path("unit.o")));
        EXPECT_FALSE(llvm::timeTraceProfilerEnabled());
    }

    INSTANTIATE_TEST_SUITE_P(Paths, TimeTraceTest,
                             ::testing::Values(TimeTraceMode{"Plain", false, nullptr},
                                               TimeTraceMode{"PlainOptNone", false, "--ct-optnone"},
                                               TimeTraceMode{"Instrumented", true,
                                                             "--ct-modules=alloc"}),
                             [](const ::testing::TestParamInfo<TimeTraceMode>& info)
                             { return std::string(info.param.name); });

    // An instrumented trace covers the instrumentation, between the optimization and the
    // code generation.
    TEST_F(CompileTest, InstrumentedTraceCoversTheInstrumentation)
    {
        compilerlib::CompileResult result = compilerlib::compile(
            {kElfTarget, "-O2", "-ftime-trace", "-ftime-trace-granularity=0", "-c",
             writeSource("unit.c", kSectionUnit), "-o", path("unit.o"), "--ct-modules=alloc"},
            compilerlib::OutputMode::ToFile, /*instrument=*/true);
        ASSERT_TRUE(result.success) << result.diagnostics;
        EXPECT_EQ(traceEvents(path("unit.json")).count("CoreTraceInstrumentation"), 1u);
    }
    // A unit whose compilation at -O2 reports a remark of each -Rpass family, from the
    // optimization (inline, loop-vectorize) and from the code generation (prologepilog), an
    // optimization failure (the loop cannot be vectorized as requested), and a frame larger
    // than -Wframe-larger-than=64, which code generation locates at the function.
    constexpr const char* kBackendDiagnosticsUnit = R"(static int square(int x)
{
    return x * x;
}

int compute(int x)
{
    return square(x) + 1;
}

__attribute__((noinline)) int opaque(int x)
{
    return x * 3;
}

int caller(int x)
{
    return opaque(x) + 2;
}

int scan(int* a, int n)
{
    int last = 0;
#pragma clang loop vectorize(enable)
    for (int i = 0; i < n; i++)
    {
        last = a[i] * 3 + last;
        a[i] = last;
    }
    return last;
}

int frame(int i)
{
    volatile char buffer[256];
    buffer[i] = 1;
    return buffer[0];
}
)";

    // The diagnostics located in `source`, as "line:column: level: message". A plain
    // compilation also prints source excerpts and the option of each diagnostic, which
    // instrumented compilations do not print.
    std::multiset<std::string> diagnosticsIn(const std::string& diagnostics,
                                             const std::string& source)
    {
        std::multiset<std::string> located;
        std::istringstream lines(diagnostics);
        std::string line;
        const std::string prefix = source + ":";
        while (std::getline(lines, line))
        {
            if (line.rfind(prefix, 0) != 0)
                continue;
            line.erase(0, prefix.size());
            const size_t option = line.rfind(" [-");
            if (option != std::string::npos && !line.empty() && line.back() == ']')
                line.erase(option);
            located.insert(line);
        }
        return located;
    }

    size_t countLevel(const std::multiset<std::string>& diagnostics, const char* level)
    {
        return static_cast<size_t>(
            std::count_if(diagnostics.begin(), diagnostics.end(), [&](const std::string& diagnostic)
                          { return diagnostic.find(level) != std::string::npos; }));
    }

    // Diagnostics of the optimization and of the code generation of instrumented code go
    // through Clang's diagnostics, as for plain code (#167): with the same filtering, the
    // same locations and the same severity, which warning options change.
    class BackendDiagnosticsTest : public CompileTest
    {
      protected:
        compilerlib::CompileResult compileUnit(bool instrument,
                                               const std::vector<std::string>& options)
        {
            source_ = writeSource("unit.c", kBackendDiagnosticsUnit);
            // Instrumented compilations add -gline-tables-only when no -g is given: plain
            // ones get it too, so that both locate diagnostics with the same debug locations.
            std::vector<std::string> args = {kElfTarget, "-O2", "-gline-tables-only"};
            args.insert(args.end(), options.begin(), options.end());
            args.insert(args.end(),
                        {"-c", source_, "-o", path(instrument ? "instrumented.o" : "plain.o")});
            if (instrument)
                args.push_back("--ct-modules=alloc");
            // Every diagnostic belongs in the result: LLVM's default handler would print to
            // the process's stderr instead, unfiltered.
            ::testing::internal::CaptureStderr();
            compilerlib::CompileResult result =
                compilerlib::compile(args, compilerlib::OutputMode::ToFile, instrument);
            EXPECT_EQ(::testing::internal::GetCapturedStderr(), "");
            return result;
        }

        // The diagnostics of the plain and of the instrumented compilation, both expected
        // to succeed.
        std::pair<std::multiset<std::string>, std::multiset<std::string>>
        plainAndInstrumented(const std::vector<std::string>& options)
        {
            compilerlib::CompileResult plain = compileUnit(false, options);
            EXPECT_TRUE(plain.success) << plain.diagnostics;
            compilerlib::CompileResult instrumented = compileUnit(true, options);
            EXPECT_TRUE(instrumented.success) << instrumented.diagnostics;
            return {diagnosticsIn(plain.diagnostics, source_),
                    diagnosticsIn(instrumented.diagnostics, source_)};
        }

        std::string source_;
    };

    class RemarkFamilyTest : public BackendDiagnosticsTest,
                             public ::testing::WithParamInterface<const char*>
    {
    };

    TEST_P(RemarkFamilyTest, InstrumentedRemarksMatchPlain)
    {
        auto [plain, instrumented] = plainAndInstrumented({GetParam()});
        EXPECT_GT(countLevel(plain, ": remark: "), 0u) << "the test's premise";
        EXPECT_EQ(instrumented, plain);
    }

    INSTANTIATE_TEST_SUITE_P(Families, RemarkFamilyTest,
                             ::testing::Values("-Rpass=inline", "-Rpass-missed=inline",
                                               "-Rpass-analysis=loop-vectorize",
                                               "-Rpass-analysis=prologepilog"),
                             [](const ::testing::TestParamInfo<const char*>& info)
                             {
                                 switch (info.index)
                                 {
                                 case 0:
                                     return std::string("Passed");
                                 case 1:
                                     return std::string("Missed");
                                 case 2:
                                     return std::string("Analysis");
                                 default:
                                     return std::string("CodeGenerationAnalysis");
                                 }
                             });

    // -Rpass=<regex> selects remarks by pass name, and remarks are off without it.
    TEST_F(BackendDiagnosticsTest, InstrumentedRemarksAreFiltered)
    {
        auto [plain, instrumented] = plainAndInstrumented({"-Rpass=inl.ne"});
        EXPECT_EQ(instrumented, plain);
        EXPECT_EQ(countLevel(instrumented, ": remark: "), 1u);

        compilerlib::CompileResult unrequested = compileUnit(true, {});
        EXPECT_TRUE(unrequested.success) << unrequested.diagnostics;
        EXPECT_EQ(unrequested.diagnostics.find("remark"), std::string::npos)
            << unrequested.diagnostics;
    }

    TEST_F(BackendDiagnosticsTest, InstrumentedOptimizationFailureMatchesPlain)
    {
        auto [plain, instrumented] = plainAndInstrumented({});
        EXPECT_EQ(countLevel(plain, ": warning: loop not vectorized"), 1u) << "the test's premise";
        EXPECT_EQ(instrumented, plain);
    }

    // Code generation locates the frame size warning at the function's declaration, which
    // the instrumented path must still know after the frontend.
    TEST_F(BackendDiagnosticsTest, InstrumentedFunctionLocationsMatchPlain)
    {
        auto [plain, instrumented] = plainAndInstrumented({"-Wframe-larger-than=64"});
        EXPECT_EQ(countLevel(plain, ": warning: stack frame size"), 1u) << "the test's premise";
        EXPECT_EQ(instrumented, plain);
    }

    TEST_F(BackendDiagnosticsTest, WerrorMakesAnInstrumentedOptimizationFailureAnError)
    {
        compilerlib::CompileResult result = compileUnit(true, {"-Werror"});
        EXPECT_FALSE(result.success);
        EXPECT_EQ(
            countLevel(diagnosticsIn(result.diagnostics, source_), ": error: loop not vectorized"),
            1u)
            << result.diagnostics;
        EXPECT_FALSE(fs::exists(path("instrumented.o")));
    }

    class SilencedOptimizationFailureTest : public BackendDiagnosticsTest,
                                            public ::testing::WithParamInterface<const char*>
    {
    };

    TEST_P(SilencedOptimizationFailureTest, IsNotPrinted)
    {
        compilerlib::CompileResult result = compileUnit(true, {GetParam()});
        EXPECT_TRUE(result.success) << result.diagnostics;
        EXPECT_EQ(result.diagnostics.find("loop not vectorized"), std::string::npos)
            << result.diagnostics;
    }

    std::string silencingOptionName(const ::testing::TestParamInfo<const char*>& info)
    {
        return info.index == 0 ? "NoWarnings" : "NoPassFailed";
    }

    INSTANTIATE_TEST_SUITE_P(Options, SilencedOptimizationFailureTest,
                             ::testing::Values("-w", "-Wno-pass-failed"), silencingOptionName);

    // The handler leaves the optimization record whole: it holds the remarks of
    // optimization and code generation, whichever -Rpass selects.
    TEST_F(BackendDiagnosticsTest, InstrumentedRecordWithRemarksMatchesPlain)
    {
        std::multiset<std::string> records[2];
        for (const bool instrument : {false, true})
        {
            compilerlib::CompileResult result =
                compileUnit(instrument, {"-Rpass=inline", "-fsave-optimization-record"});
            ASSERT_TRUE(result.success) << result.diagnostics;
            records[instrument] =
                recordedRemarks(path(instrument ? "instrumented.opt.yaml" : "plain.opt.yaml"));
        }
        EXPECT_FALSE(records[false].empty()) << "the test's premise";
        EXPECT_EQ(records[true], records[false]);
    }

    // A code generation error is reported where Clang reports it, and fails the compilation.
    TEST_F(BackendDiagnosticsTest, InstrumentedCodeGenerationErrorMatchesPlain)
    {
        const std::string source = CT_TEST_SOURCE_DIR "/examples/fixtures/codegen_error.c";
        std::multiset<std::string> errors[2];
        for (const bool instrument : {false, true})
        {
            ::testing::internal::CaptureStderr();
            compilerlib::CompileResult result = compilerlib::compile(
                {kElfTarget, "-c", source, "-o", path(instrument ? "instrumented.o" : "plain.o")},
                compilerlib::OutputMode::ToFile, instrument);
            EXPECT_EQ(::testing::internal::GetCapturedStderr(), "");
            EXPECT_FALSE(result.success);
            errors[instrument] = diagnosticsIn(result.diagnostics, source);
        }
        EXPECT_EQ(countLevel(errors[false], ": error: "), 1u) << "the test's premise";
        EXPECT_EQ(errors[true], errors[false]);
    }
} // namespace
