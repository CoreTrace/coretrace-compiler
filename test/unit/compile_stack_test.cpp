// SPDX-License-Identifier: Apache-2.0
//
// Clang recurses deeply, in template instantiation among others: a host that compiles from a
// thread of its own, whose stack may be far smaller than the 8 MiB Clang wants (512 KiB for a
// secondary thread on macOS), must not overflow it (#204). Each compilation here runs on a thread
// with a 512 KiB stack, in the subprocess of a death test, so that an overflow kills that
// subprocess and not the suite. POSIX only, for pthread_attr_setstacksize.
#include "compilerlib/compiler.h"

#include <gtest/gtest.h>

#include <llvm/Support/CrashRecoveryContext.h>

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

    // Diagnostics and failures come back from the thread Clang runs on as they did before: a
    // warning with a successful compilation, an error with a failed one that leaves no output,
    // in each mode. Checked in the subprocess, which exits with the number of mismatches.
    void checkDiagnosticsAndExit(const std::string& warns, const std::string& fails,
                                 const std::string& dir)
    {
        const std::string warnsObject = dir + "/warns.o";
        const std::string warnsInstrumented = dir + "/warns-instrumented.o";
        const std::string failsObject = dir + "/fails.o";
        const std::string failsInstrumented = dir + "/fails-instrumented.o";
        std::vector<Compilation> jobs{
            {{"-std=c++17", "-S", "-emit-llvm", warns},
             compilerlib::OutputMode::ToMemory,
             false,
             {}},
            {{"-std=c++17", "-c", warns, "-o", warnsObject},
             compilerlib::OutputMode::ToFile,
             false,
             {}},
            {{"-std=c++17", "-c", warns, "-o", warnsInstrumented},
             compilerlib::OutputMode::ToFile,
             true,
             {}},
            {{"-std=c++17", "-S", "-emit-llvm", fails},
             compilerlib::OutputMode::ToMemory,
             false,
             {}},
            {{"-std=c++17", "-c", fails, "-o", failsObject},
             compilerlib::OutputMode::ToFile,
             false,
             {}},
            {{"-std=c++17", "-c", fails, "-o", failsInstrumented},
             compilerlib::OutputMode::ToFile,
             true,
             {}},
        };
        for (Compilation& job : jobs)
        {
            std::vector<Compilation> one{job};
            compileOnSmallStacks(one);
            job.result = one.front().result;
        }

        int mismatches = 0;
        auto expect = [&](bool ok, const char* what, size_t job)
        {
            if (!ok)
            {
                std::fprintf(stderr, "job %zu: %s\n%s\n", job, what,
                             job < jobs.size() ? jobs[job].result.diagnostics.c_str() : "");
                ++mismatches;
            }
        };
        for (size_t i = 0; i < 3; ++i)
        {
            expect(jobs[i].result.success, "failed", i);
            expect(jobs[i].result.diagnostics.find("deep but fine") != std::string::npos,
                   "no warning", i);
        }
        expect(jobs[0].result.llvmIR.find("define") != std::string::npos, "no IR", 0);
        expect(fs::exists(warnsObject), "no object", 1);
        expect(fs::exists(warnsInstrumented), "no object", 2);
        for (size_t i = 3; i < jobs.size(); ++i)
        {
            expect(!jobs[i].result.success, "succeeded", i);
            expect(jobs[i].result.diagnostics.find("missing") != std::string::npos, "no error", i);
        }
        expect(!fs::exists(failsObject), "left an object", 4);
        expect(!fs::exists(failsInstrumented), "left an object", 5);
        std::_Exit(mismatches);
    }

    TEST_F(CompileStackTest, DiagnosticsAndFailuresComeBackFromClang)
    {
        const std::string warns =
            writeSource("warns.cpp", std::string(kDeepTemplate) + "#warning \"deep but fine\"\n");
        const std::string fails =
            writeSource("fails.cpp", std::string(kDeepTemplate) + "int broken = missing;\n");
        EXPECT_EXIT(checkDiagnosticsAndExit(warns, fails, path("")), ::testing::ExitedWithCode(0),
                    "");
    }

    // Several hosts' threads compile at once, each on a small stack: every compilation succeeds,
    // with its own diagnostics only.
    void compileAtOnceAndExit(const std::vector<std::string>& sources)
    {
        std::vector<Compilation> jobs;
        for (size_t i = 0; i < sources.size(); ++i)
        {
            jobs.push_back({{"-std=c++17", "-S", "-emit-llvm", sources[i]},
                            compilerlib::OutputMode::ToMemory,
                            i % 2 == 1,
                            {}});
        }
        compileOnSmallStacks(jobs);
        int wrong = 0;
        for (size_t i = 0; i < jobs.size(); ++i)
        {
            const std::string own = "unit " + std::to_string(i) + " ";
            bool others = false;
            for (size_t j = 0; j < jobs.size(); ++j)
            {
                others = others ||
                         (j != i && jobs[i].result.diagnostics.find("unit " + std::to_string(j) +
                                                                    " ") != std::string::npos);
            }
            if (!jobs[i].result.success ||
                jobs[i].result.diagnostics.find(own) == std::string::npos || others)
            {
                std::fprintf(stderr, "job %zu:\n%s\n", i, jobs[i].result.diagnostics.c_str());
                ++wrong;
            }
        }
        std::_Exit(wrong == 0 ? 0 : 1);
    }

    TEST_F(CompileStackTest, CompilationsAtOnceOnSmallStacks)
    {
        std::vector<std::string> sources;
        for (int i = 0; i < 8; ++i)
        {
            sources.push_back(writeSource("unit" + std::to_string(i) + ".cpp",
                                          std::string(kDeepTemplate) + "#warning \"unit " +
                                              std::to_string(i) + " \"\n"));
        }
        EXPECT_EXIT(compileAtOnceAndExit(sources), ::testing::ExitedWithCode(0), "");
    }

    // Once the host enabled crash recovery, a crash in a job fails the compilation, and the
    // job's thread still ends with the alternate signal stack it started with. In the death
    // test's subprocess, this job is the first to write a file, so LLVM installs its own
    // alternate stack on the job's thread, which AddressSanitizer would abort on.
    void crashAndExit(const std::string& source, const std::string& object)
    {
        llvm::CrashRecoveryContext::Enable();
        std::vector<Compilation> jobs{
            {{"-c", source, "-o", object}, compilerlib::OutputMode::ToFile, false, {}}};
        compileOnSmallStacks(jobs);
        std::_Exit(jobs.front().result.success ? 1 : 0);
    }

    TEST_F(CompileStackTest, ARecoveredCrashFailsTheCompilation)
    {
        const std::string source = writeSource("crash.c", "#pragma clang __debug crash\n");
        EXPECT_EXIT(crashAndExit(source, path("crash.o")), ::testing::ExitedWithCode(0), "");
    }
} // namespace
