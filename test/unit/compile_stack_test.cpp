// SPDX-License-Identifier: Apache-2.0
//
// Clang recurses deeply, in template instantiation among others: a host that compiles from a
// thread of its own, whose stack may be far smaller than the 8 MiB Clang wants (512 KiB for a
// secondary thread on macOS), must not overflow it (#204). Each compilation here runs on a thread
// with a 512 KiB stack, in the subprocess of a death test, so that an overflow kills that
// subprocess and not the suite. POSIX only, for pthread_attr_setstacksize.
#include "compilerlib/compiler.h"

#include <gtest/gtest.h>

#include <pthread.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr size_t kSmallStack = size_t{512} << 10;

    // A template instantiated a thousand levels deep, as -ftemplate-depth allows by default.
    constexpr const char* kDeepTemplate = R"(template <int N> struct Deep
{
    static constexpr int value = Deep<N - 1>::value + 1;
};

template <> struct Deep<0>
{
    static constexpr int value = 0;
};

int depth()
{
    return Deep<1000>::value;
}
)";

    struct Compilation
    {
        std::vector<std::string> args;
        compilerlib::OutputMode mode;
        bool instrument;
        compilerlib::CompileResult result;
    };

    void* compileOnThisThread(void* compilation)
    {
        auto* job = static_cast<Compilation*>(compilation);
        job->result = compilerlib::compile(job->args, job->mode, job->instrument);
        return nullptr;
    }

    // Runs every compilation at once, each on a thread of its own with a 512 KiB stack.
    void compileOnSmallStacks(std::vector<Compilation>& jobs)
    {
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, kSmallStack);
        std::vector<pthread_t> threads(jobs.size());
        for (size_t i = 0; i < jobs.size(); ++i)
        {
            if (pthread_create(&threads[i], &attributes, compileOnThisThread, &jobs[i]) != 0)
            {
                std::fprintf(stderr, "pthread_create failed\n");
                std::_Exit(2);
            }
        }
        for (pthread_t thread : threads)
        {
            pthread_join(thread, nullptr);
        }
        pthread_attr_destroy(&attributes);
    }

    // The body of a death test: compiles on a small stack and exits with 0 when it succeeded,
    // its diagnostics printed otherwise.
    void compileOnASmallStackAndExit(std::vector<std::string> args, compilerlib::OutputMode mode,
                                     bool instrument)
    {
        std::vector<Compilation> jobs{{std::move(args), mode, instrument, {}}};
        compileOnSmallStacks(jobs);
        if (!jobs.front().result.success)
        {
            std::fprintf(stderr, "compilation failed:\n%s\n",
                         jobs.front().result.diagnostics.c_str());
            std::_Exit(1);
        }
        std::_Exit(0);
    }

    // Sources and outputs in a directory of the test's own. The death tests run their bodies in a
    // subprocess that runs this test alone again, safe with the threads Clang may have started.
    class CompileStackTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            GTEST_FLAG_SET(death_test_style, "threadsafe");
            const ::testing::TestInfo* test =
                ::testing::UnitTest::GetInstance()->current_test_info();
            std::string name = std::string("ct_") + test->test_suite_name() + "." + test->name();
            std::replace(name.begin(), name.end(), '/', '_');
            dir_ = fs::path(::testing::TempDir()) / name;
            fs::create_directories(dir_);
        }

        void TearDown() override
        {
            std::error_code ignored;
            fs::remove_all(dir_, ignored);
        }

        std::string path(const std::string& name) const
        {
            return (dir_ / name).string();
        }

        std::string writeSource(const std::string& name, const std::string& text) const
        {
            std::ofstream(path(name)) << text;
            return path(name);
        }

      private:
        fs::path dir_;
    };

    // In memory, as the analyzers of CoreTrace compile (Cc1Runner::runSingle).
    TEST_F(CompileStackTest, InMemoryCompilationOnASmallStack)
    {
        const std::string source = writeSource("deep.cpp", kDeepTemplate);
        EXPECT_EXIT(compileOnASmallStackAndExit({"-std=c++17", "-S", "-emit-llvm", source},
                                                compilerlib::OutputMode::ToMemory, false),
                    ::testing::ExitedWithCode(0), "");
    }

    // To a file, through the driver, which runs its -cc1 job in this process.
    TEST_F(CompileStackTest, CompilationToAFileOnASmallStack)
    {
        const std::string source = writeSource("deep.cpp", kDeepTemplate);
        EXPECT_EXIT(compileOnASmallStackAndExit({"-std=c++17", "-c", source, "-o", path("deep.o")},
                                                compilerlib::OutputMode::ToFile, false),
                    ::testing::ExitedWithCode(0), "");
    }

    // Instrumented, to a file (Cc1Runner::runInstrumented).
    TEST_F(CompileStackTest, InstrumentedCompilationOnASmallStack)
    {
        const std::string source = writeSource("deep.cpp", kDeepTemplate);
        EXPECT_EXIT(compileOnASmallStackAndExit({"-std=c++17", "-c", source, "-o", path("deep.o")},
                                                compilerlib::OutputMode::ToFile, true),
                    ::testing::ExitedWithCode(0), "");
    }
} // namespace
