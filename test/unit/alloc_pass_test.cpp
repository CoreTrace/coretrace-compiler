// SPDX-License-Identifier: Apache-2.0
// Runs the allocation pass on hand-written IR and checks which calls it rewrites: an
// allocation is tracked when the call itself is user code, and a release is rewritten
// wherever it is, so that a tracked block released by a system header is seen. Then
// which allocations it releases before the function returns (auto-free): only those
// that cannot be used after the return, and only with valid IR.
#include "compilerlib/instrumentation/alloc.hpp"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
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
#include <utility>
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

    class InstrumentedModuleTest : public ::testing::Test
    {
      protected:
        // Parses `ir` and runs the allocation pass on it, which must leave valid IR.
        void instrument(const char* ir)
        {
            llvm::SMDiagnostic parseError;
            module_ = llvm::parseAssemblyString(ir, parseError, context_);
            ASSERT_NE(module_, nullptr) << parseError.getMessage().str();
            compilerlib::wrapAllocCalls(*module_);

            std::string problems;
            llvm::raw_string_ostream stream(problems);
            ASSERT_FALSE(llvm::verifyModule(*module_, &stream)) << stream.str();
        }

        // Names of the functions @function calls, in order, including those called with
        // another function type than they are declared with, as clang calls Objective-C
        // selector stubs.
        std::vector<std::string> callees(const char* function) const
        {
            std::vector<std::string> names;
            for (llvm::Instruction& inst : llvm::instructions(*module_->getFunction(function)))
            {
                if (auto* call = llvm::dyn_cast<llvm::CallBase>(&inst))
                {
                    if (auto* callee = llvm::dyn_cast<llvm::Function>(
                            call->getCalledOperand()->stripPointerCasts()))
                        names.push_back(callee->getName().str());
                }
            }
            return names;
        }

        llvm::LLVMContext context_;
        std::unique_ptr<llvm::Module> module_;
    };

    class AllocPassTest : public InstrumentedModuleTest
    {
      protected:
        void SetUp() override
        {
            instrument(kModule);
        }
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

    // Before the module's static initializers, which may register exit-time code that
    // releases blocks: programs may give their own constructors priority 101 and above.
    TEST_F(AllocPassTest, ModuleSchedulesTheLeakReportBeforeItsConstructors)
    {
        const llvm::GlobalVariable* ctors = module_->getGlobalVariable("llvm.global_ctors");
        ASSERT_NE(ctors, nullptr);
        const auto* entries = llvm::dyn_cast<llvm::ConstantArray>(ctors->getInitializer());
        ASSERT_NE(entries, nullptr);
        int scheduled = 0;
        for (const llvm::Use& use : entries->operands())
        {
            const auto* entry = llvm::cast<llvm::ConstantStruct>(use.get());
            const auto* function = llvm::dyn_cast<llvm::Function>(entry->getOperand(1));
            if (function && function->getName() == "__ct_schedule_leak_report")
            {
                ++scheduled;
                EXPECT_LT(llvm::cast<llvm::ConstantInt>(entry->getOperand(0))->getZExtValue(),
                          101u);
            }
        }
        EXPECT_EQ(scheduled, 1);
    }

    using AutoFreeTest = InstrumentedModuleTest;

    TEST_F(AutoFreeTest, AllocationUsedOnlyInItsFunctionIsReleasedBeforeTheReturn)
    {
        instrument(R"(
declare ptr @malloc(i64)

define void @local() {
  %block = call ptr @malloc(i64 8)
  store i8 1, ptr %block
  ret void
}
)");
        EXPECT_EQ(callees("local"), (std::vector<std::string>{"__ct_malloc", "__ct_autofree"}));
    }

    // An unused allocation by an invoke, as in a function with cleanups: the release goes
    // where the call returns normally, the only place its result is available.
    TEST_F(AutoFreeTest, UnusedInvokedAllocationIsReleasedOnTheNormalPath)
    {
        instrument(R"(
declare ptr @malloc(i64)
declare i32 @__gxx_personality_v0(...)

define void @unused_invoked() personality ptr @__gxx_personality_v0 {
entry:
  %block = invoke ptr @malloc(i64 8)
          to label %done unwind label %failed

done:
  ret void

failed:
  %pad = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %pad
}
)");
        EXPECT_EQ(callees("unused_invoked"),
                  (std::vector<std::string>{"__ct_malloc_unreachable", "__ct_autofree"}));
        for (llvm::Instruction& inst : llvm::instructions(*module_->getFunction("unused_invoked")))
        {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst))
                EXPECT_EQ(call->getParent()->getName(), "done");
        }
    }

    // The same, when the normal destination has another predecessor and a PHI: the release
    // goes into a block of its own on the invoke's edge, which the PHI then comes from.
    TEST_F(AutoFreeTest, UnusedInvokedAllocationIsReleasedOnASplitEdge)
    {
        instrument(R"(
declare ptr @malloc(i64)
declare i32 @__gxx_personality_v0(...)

define i32 @unused_invoked_shared(i1 %allocate) personality ptr @__gxx_personality_v0 {
entry:
  br i1 %allocate, label %call, label %join

call:
  %block = invoke ptr @malloc(i64 8)
          to label %join unwind label %failed

join:
  %result = phi i32 [ 1, %call ], [ 0, %entry ]
  ret i32 %result

failed:
  %pad = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %pad
}
)");
        llvm::Function& function = *module_->getFunction("unused_invoked_shared");
        EXPECT_EQ(callees("unused_invoked_shared"),
                  (std::vector<std::string>{"__ct_malloc_unreachable", "__ct_autofree"}));
        llvm::BasicBlock* release = nullptr;
        for (llvm::Instruction& inst : llvm::instructions(function))
        {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst))
                release = call->getParent();
        }
        ASSERT_NE(release, nullptr);
        llvm::BasicBlock* call = nullptr;
        llvm::BasicBlock* join = nullptr;
        for (llvm::BasicBlock& block : function)
        {
            if (block.getName() == "call")
                call = &block;
            else if (block.getName() == "join")
                join = &block;
        }
        ASSERT_NE(call, nullptr);
        ASSERT_NE(join, nullptr);
        EXPECT_EQ(release->getSinglePredecessor(), call);
        EXPECT_EQ(release->getSingleSuccessor(), join);
        auto* phi = llvm::cast<llvm::PHINode>(&join->front());
        EXPECT_GE(phi->getBasicBlockIndex(release), 0);
        EXPECT_LT(phi->getBasicBlockIndex(call), 0);
    }

    TEST_F(AutoFreeTest, AllocationCopiedBetweenLocalSlotsIsReleasedBeforeTheReturn)
    {
        instrument(R"(
declare ptr @malloc(i64)

define void @local_copy() {
  %first = alloca ptr
  %second = alloca ptr
  %block = call ptr @malloc(i64 8)
  store ptr %block, ptr %first
  %copy = load ptr, ptr %first
  store ptr %copy, ptr %second
  ret void
}
)");
        EXPECT_EQ(callees("local_copy"),
                  (std::vector<std::string>{"__ct_malloc", "__ct_autofree"}));
    }

    // Clang at -O0: `struct node* head = node; return head;`.
    TEST_F(AutoFreeTest, AllocationReturnedThroughASecondLocalSlotIsKept)
    {
        instrument(R"(
declare ptr @malloc(i64)

define ptr @make() {
  %node = alloca ptr
  %head = alloca ptr
  %block = call ptr @malloc(i64 16)
  store ptr %block, ptr %node
  %copy = load ptr, ptr %node
  store ptr %copy, ptr %head
  %result = load ptr, ptr %head
  ret ptr %result
}
)");
        EXPECT_EQ(callees("make"), std::vector<std::string>{"__ct_malloc"});
    }

    // Clang at -O0: a list built in a loop, `node->next = head; head = node;`.
    TEST_F(AutoFreeTest, ListBuiltInALoopIsKept)
    {
        instrument(R"(
declare ptr @malloc(i64)

define ptr @build(i32 %length) {
entry:
  %head = alloca ptr
  %node = alloca ptr
  %i = alloca i32
  store ptr null, ptr %head
  store i32 0, ptr %i
  br label %cond

cond:
  %n = load i32, ptr %i
  %more = icmp slt i32 %n, %length
  br i1 %more, label %body, label %done

body:
  %block = call ptr @malloc(i64 8)
  store ptr %block, ptr %node
  %previous = load ptr, ptr %head
  %current = load ptr, ptr %node
  %next_field = getelementptr inbounds { ptr }, ptr %current, i32 0, i32 0
  store ptr %previous, ptr %next_field
  %copy = load ptr, ptr %node
  store ptr %copy, ptr %head
  %incremented = add i32 %n, 1
  store i32 %incremented, ptr %i
  br label %cond

done:
  %result = load ptr, ptr %head
  ret ptr %result
}
)");
        EXPECT_EQ(callees("build"), std::vector<std::string>{"__ct_malloc"});
    }

    TEST_F(AutoFreeTest, LocalSlotsCopiedIntoEachOtherAreClassified)
    {
        instrument(R"(
declare ptr @malloc(i64)

define void @swap() {
  %first = alloca ptr
  %second = alloca ptr
  %block = call ptr @malloc(i64 8)
  store ptr %block, ptr %first
  %to_second = load ptr, ptr %first
  store ptr %to_second, ptr %second
  %to_first = load ptr, ptr %second
  store ptr %to_first, ptr %first
  ret void
}
)");
        ASSERT_FALSE(callees("swap").empty());
        EXPECT_EQ(callees("swap").front(), "__ct_malloc");
    }

    // Clang at -O2: an allocation in a branch, which does not reach the return on every
    // path. Releasing it there would use a value that does not dominate the return.
    TEST_F(AutoFreeTest, AllocationInABranchIsNotReleasedAtTheReturn)
    {
        instrument(R"(
declare ptr @malloc(i64)

define i32 @conditional(i32 %n) {
entry:
  %positive = icmp sgt i32 %n, 0
  br i1 %positive, label %allocate, label %done

allocate:
  %block = call ptr @malloc(i64 8)
  %byte = trunc i32 %n to i8
  store i8 %byte, ptr %block
  br label %done

done:
  %result = phi i32 [ %n, %allocate ], [ 0, %entry ]
  ret i32 %result
}
)");
        EXPECT_EQ(callees("conditional"), std::vector<std::string>{"__ct_malloc"});
    }

    // Clang marks a new-expression's operator new and delete calls builtin; before
    // optimization, the calls the pass tracks lose that mark. Aligned operator new is not
    // tracked, and keeps it.
    TEST(KeepTrackedAllocationCalls, UnmarksTrackedOperatorCallsOnly)
    {
        llvm::LLVMContext context;
        llvm::SMDiagnostic parseError;
        std::unique_ptr<llvm::Module> module = llvm::parseAssemblyString(R"(
declare nonnull ptr @_Znwm(i64) nobuiltin
declare void @_ZdlPvm(ptr, i64) nobuiltin
declare nonnull ptr @_ZnwmSt11align_val_t(i64, i64) nobuiltin
declare ptr @malloc(i64)

define void @allocate() {
  %object = call ptr @_Znwm(i64 4) #0
  call void @_ZdlPvm(ptr %object, i64 4) #0
  %wide = call ptr @_ZnwmSt11align_val_t(i64 64, i64 64) #0
  %block = call ptr @malloc(i64 4)
  ret void
}

attributes #0 = { builtin }
)",
                                                                         parseError, context);
        ASSERT_NE(module, nullptr) << parseError.getMessage().str();
        compilerlib::keepTrackedAllocationCalls(*module);

        std::vector<std::pair<std::string, bool>> builtinCalls;
        for (llvm::Instruction& inst : llvm::instructions(*module->getFunction("allocate")))
        {
            if (auto* call = llvm::dyn_cast<llvm::CallBase>(&inst))
            {
                builtinCalls.emplace_back(
                    call->getCalledFunction()->getName().str(),
                    call->getAttributes().hasFnAttr(llvm::Attribute::Builtin));
            }
        }
        EXPECT_EQ(builtinCalls, (std::vector<std::pair<std::string, bool>>{
                                    {"_Znwm", false},
                                    {"_ZdlPvm", false},
                                    {"_ZnwmSt11align_val_t", true},
                                    {"malloc", false},
                                }));
    }

    using ObjcAllocationTest = InstrumentedModuleTest;

    // From Clang 23, Apple targets send messages through selector stubs,
    // objc_msgSend$<selector>: the receiver comes first and no selector argument is
    // passed. An allocation sent through a stub is tracked, another message is not.
    TEST_F(ObjcAllocationTest, AllocationSentThroughASelectorStubIsTracked)
    {
        instrument(R"(
target triple = "arm64-apple-macosx15.0.0"

declare ptr @"objc_msgSend$new"(ptr, ptr, ...)
declare ptr @"objc_msgSend$class"(ptr, ptr, ...)
declare ptr @"objc_msgSend$allocWithZone:"(ptr, ptr, ...)

define void @make(ptr %class, ptr %zone) {
  %byNew = call ptr @"objc_msgSend$new"(ptr %class, ptr undef)
  %metaclass = call ptr @"objc_msgSend$class"(ptr %byNew, ptr undef)
  %byZone = call ptr @"objc_msgSend$allocWithZone:"(ptr %metaclass, ptr undef, ptr %zone)
  ret void
}
)");
        EXPECT_EQ(callees("make"), (std::vector<std::string>{
                                       "objc_msgSend$new", "__ct_objc_track", "objc_msgSend$class",
                                       "objc_msgSend$allocWithZone:", "__ct_objc_track"}));
    }
} // namespace
