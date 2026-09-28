// SPDX-License-Identifier: Apache-2.0
// Runs the allocation pass on hand-written IR and checks which calls it rewrites: an
// allocation is tracked when the call itself is user code, and a release is rewritten
// wherever it is, so that a tracked block released by a system header is seen.
#include "compilerlib/instrumentation/alloc.hpp"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace
{
    // user.cpp holds the user functions; allocate.h, under a system include directory,
    // the library's. The call in @inlined is a library call inlined into user code.
    constexpr const char* kModule = R"(
declare ptr @_Znwm(i64)
declare void @_ZdlPv(ptr)

define ptr @user() !dbg !10 {
  %block = call ptr @_Znwm(i64 4), !dbg !11
  ret ptr %block
}

define ptr @inlined() !dbg !12 {
  %block = call ptr @_Znwm(i64 4), !dbg !14
  ret ptr %block
}

define linkonce_odr ptr @library_allocate() !dbg !16 {
  %block = call ptr @_Znwm(i64 4), !dbg !17
  ret ptr %block
}

define linkonce_odr void @library_release(ptr %block) !dbg !18 {
  call void @_ZdlPv(ptr %block), !dbg !19
  ret void
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2}

!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus_14, file: !1, emissionKind: LineTablesOnly)
!1 = !DIFile(filename: "user.cpp", directory: "/work")
!2 = !{i32 2, !"Debug Info Version", i32 3}
!3 = !DIFile(filename: "__memory/allocate.h", directory: "/usr/include/c++/v1")
!4 = !DISubroutineType(types: !{})
!10 = distinct !DISubprogram(name: "user", scope: !1, file: !1, line: 1, type: !4, spFlags: DISPFlagDefinition, unit: !0)
!11 = !DILocation(line: 2, column: 3, scope: !10)
!12 = distinct !DISubprogram(name: "inlined", scope: !1, file: !1, line: 5, type: !4, spFlags: DISPFlagDefinition, unit: !0)
!13 = distinct !DISubprogram(name: "allocate", scope: !3, file: !3, line: 30, type: !4, spFlags: DISPFlagDefinition, unit: !0)
!14 = !DILocation(line: 37, column: 10, scope: !13, inlinedAt: !15)
!15 = !DILocation(line: 6, column: 3, scope: !12)
!16 = distinct !DISubprogram(name: "library_allocate", scope: !3, file: !3, line: 40, type: !4, spFlags: DISPFlagDefinition, unit: !0)
!17 = !DILocation(line: 41, column: 3, scope: !16)
!18 = distinct !DISubprogram(name: "library_release", scope: !3, file: !3, line: 50, type: !4, spFlags: DISPFlagDefinition, unit: !0)
!19 = !DILocation(line: 51, column: 3, scope: !18)
)";

    class AllocPassTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            llvm::SMDiagnostic parseError;
            module_ = llvm::parseAssemblyString(kModule, parseError, context_);
            ASSERT_NE(module_, nullptr) << parseError.getMessage().str();
            compilerlib::wrapAllocCalls(*module_);

            std::string problems;
            llvm::raw_string_ostream stream(problems);
            ASSERT_FALSE(llvm::verifyModule(*module_, &stream)) << stream.str();
        }

        // Names of the functions @function calls, in order.
        std::vector<std::string> callees(const char* function) const
        {
            std::vector<std::string> names;
            for (llvm::Instruction& inst : llvm::instructions(*module_->getFunction(function)))
            {
                if (auto* call = llvm::dyn_cast<llvm::CallBase>(&inst))
                {
                    if (llvm::Function* callee = call->getCalledFunction())
                        names.push_back(callee->getName().str());
                }
            }
            return names;
        }

        llvm::LLVMContext context_;
        std::unique_ptr<llvm::Module> module_;
    };

    TEST_F(AllocPassTest, UserAllocationIsTracked)
    {
        EXPECT_EQ(callees("user"), std::vector<std::string>{"__ct_new"});
    }

    TEST_F(AllocPassTest, LibraryAllocationInlinedIntoUserCodeStaysUntracked)
    {
        EXPECT_EQ(callees("inlined"), std::vector<std::string>{"_Znwm"});
    }

    TEST_F(AllocPassTest, LibraryAllocationStaysUntracked)
    {
        EXPECT_EQ(callees("library_allocate"), std::vector<std::string>{"_Znwm"});
    }

    TEST_F(AllocPassTest, LibraryReleaseGoesThroughTheRuntime)
    {
        EXPECT_EQ(callees("library_release"), std::vector<std::string>{"__ct_delete"});
    }
} // namespace
