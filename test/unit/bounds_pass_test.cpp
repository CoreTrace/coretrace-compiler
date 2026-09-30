// SPDX-License-Identifier: Apache-2.0
// Runs the bounds pass on hand-written IR and checks the base each access is checked
// against, and how a frame keeps the registry of stack objects around setjmp.
#include "compilerlib/instrumentation/bounds.hpp"

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

namespace
{
    class BoundsPassTest : public ::testing::Test
    {
      protected:
        // Parses the module, runs the bounds pass on it and requires valid IR. Returns the
        // module's function @f, or null when the IR does not parse.
        llvm::Function* instrument(const char* ir)
        {
            llvm::SMDiagnostic parseError;
            module_ = llvm::parseAssemblyString(ir, parseError, context_);
            if (!module_)
            {
                ADD_FAILURE() << parseError.getMessage().str();
                return nullptr;
            }
            compilerlib::instrumentMemoryAccesses(*module_);

            std::string problems;
            llvm::raw_string_ostream stream(problems);
            EXPECT_FALSE(llvm::verifyModule(*module_, &stream)) << stream.str();
            return module_->getFunction("f");
        }

        static llvm::Value* named(llvm::Function& func, llvm::StringRef name)
        {
            for (llvm::Argument& arg : func.args())
            {
                if (arg.getName() == name)
                    return &arg;
            }
            for (llvm::Instruction& inst : llvm::instructions(func))
            {
                if (inst.getName() == name)
                    return &inst;
            }
            return nullptr;
        }

        // Name of the base the bounds check of an access through `accessed` uses.
        static std::string checkedBase(llvm::Function& func, llvm::StringRef accessed)
        {
            llvm::Value* pointer = named(func, accessed);
            for (llvm::Instruction& inst : llvm::instructions(func))
            {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                llvm::Function* callee = call ? call->getCalledFunction() : nullptr;
                if (callee && callee->getName() == "__ct_check_bounds" &&
                    call->getArgOperand(1) == pointer)
                {
                    llvm::Value* base = call->getArgOperand(0);
                    return base->hasName() ? base->getName().str() : "<unnamed value>";
                }
            }
            return "<no check>";
        }

        // The runtime calls around the call to `callee` in @f, in order: "depth" for the
        // depth read right before it, "pop" when the next instruction restores that depth.
        static std::string stackDepthAround(llvm::Function& func, llvm::StringRef callee)
        {
            for (llvm::Instruction& inst : llvm::instructions(func))
            {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                if (!call || !call->getCalledFunction() ||
                    call->getCalledFunction()->getName() != callee)
                {
                    continue;
                }
                std::string around;
                auto* before = llvm::dyn_cast_or_null<llvm::CallInst>(call->getPrevNode());
                if (before && before->getCalledFunction() &&
                    before->getCalledFunction()->getName() == "__ct_stack_depth")
                {
                    around += "depth";
                }
                auto* after = llvm::dyn_cast_or_null<llvm::CallInst>(call->getNextNode());
                if (after && after->getCalledFunction() &&
                    after->getCalledFunction()->getName() == "__ct_stack_pop" &&
                    after->getArgOperand(0) == before)
                {
                    around += around.empty() ? "pop" : ",pop";
                }
                return around.empty() ? "<nothing>" : around;
            }
            return "<no call>";
        }

        llvm::LLVMContext context_;
        std::unique_ptr<llvm::Module> module_;
    };

    // A pointer loaded from a slot that holds only that pointer is checked against the
    // pointer stored there.
    TEST_F(BoundsPassTest, PointerLoadedFromItsSlotIsCheckedAgainstTheStoredPointer)
    {
        llvm::Function* func = instrument(R"(
define i32 @f(ptr %block) {
  %slot = alloca ptr, align 8
  store ptr %block, ptr %slot, align 8
  %p = load ptr, ptr %slot, align 8
  %element = getelementptr inbounds i32, ptr %p, i64 1
  %value = load i32, ptr %element, align 4
  ret i32 %value
}
)");
        ASSERT_NE(func, nullptr);
        EXPECT_EQ(checkedBase(*func, "element"), "block");
    }

    // The vectorizer copies a two-pointer object, such as a std::shared_ptr, with one
    // store of a pointer vector: the pointer loaded back is not that vector.
    TEST_F(BoundsPassTest, PointerLoadedFromASlotStoredAsAPointerVectorIsItsOwnBase)
    {
        llvm::Function* func = instrument(R"(
define i32 @f(ptr %source) {
  %slot = alloca { ptr, ptr }, align 16
  %pair = load <2 x ptr>, ptr %source, align 16
  store <2 x ptr> %pair, ptr %slot, align 16
  %p = load ptr, ptr %slot, align 16
  %value = load i32, ptr %p, align 4
  ret i32 %value
}
)");
        ASSERT_NE(func, nullptr);
        EXPECT_EQ(checkedBase(*func, "p"), "p");
    }

    // A pointer loaded from the second field of a slot is not the value stored in its
    // first field.
    TEST_F(BoundsPassTest, PointerLoadedFromALaterFieldIsNotTheFirstFieldsValue)
    {
        llvm::Function* func = instrument(R"(
define i32 @f(ptr %first, ptr %second) {
  %slot = alloca { ptr, ptr }, align 8
  store ptr %first, ptr %slot, align 8
  %field = getelementptr inbounds i8, ptr %slot, i64 8
  store ptr %second, ptr %field, align 8
  %p = load ptr, ptr %field, align 8
  %value = load i32, ptr %p, align 4
  ret i32 %value
}
)");
        ASSERT_NE(func, nullptr);
        EXPECT_EQ(checkedBase(*func, "p"), "p");
    }

    // A callee that receives the slot's address may store another pointer in it.
    TEST_F(BoundsPassTest, PointerLoadedFromASlotACalleeMayWriteIsItsOwnBase)
    {
        llvm::Function* func = instrument(R"(
declare void @replace(ptr)

define i32 @f(ptr %first) {
  %slot = alloca ptr, align 8
  store ptr %first, ptr %slot, align 8
  call void @replace(ptr %slot)
  %p = load ptr, ptr %slot, align 8
  %value = load i32, ptr %p, align 4
  ret i32 %value
}
)");
        ASSERT_NE(func, nullptr);
        EXPECT_EQ(checkedBase(*func, "p"), "p");
    }

    // setjmp returns a second time from a longjmp that left the frames above it without
    // their exits: the depth read before the call is restored after it.
    TEST_F(BoundsPassTest, DepthIsRestoredAfterACallThatReturnsTwice)
    {
        llvm::Function* func = instrument(R"(
declare i32 @setjmp(ptr) returns_twice

define i32 @f(ptr %buffer) {
  %again = call i32 @setjmp(ptr %buffer) returns_twice
  ret i32 %again
}
)");
        ASSERT_NE(func, nullptr);
        EXPECT_EQ(stackDepthAround(*func, "setjmp"), "depth,pop");
    }

    // Instrumented code is compiled with -fno-builtin, which keeps clang from marking the C
    // library's setjmp returns_twice.
    TEST_F(BoundsPassTest, DepthIsRestoredAfterSetjmpWithoutTheAttribute)
    {
        llvm::Function* func = instrument(R"(
declare i32 @_setjmp(ptr)

define i32 @f(ptr %buffer) {
  %again = call i32 @_setjmp(ptr %buffer)
  ret i32 %again
}
)");
        ASSERT_NE(func, nullptr);
        EXPECT_EQ(stackDepthAround(*func, "_setjmp"), "depth,pop");
    }

    TEST_F(BoundsPassTest, DepthIsLeftAloneAroundOrdinaryCalls)
    {
        llvm::Function* func = instrument(R"(
declare i32 @compute(ptr)

define i32 @f(ptr %buffer) {
  %value = call i32 @compute(ptr %buffer)
  ret i32 %value
}
)");
        ASSERT_NE(func, nullptr);
        EXPECT_EQ(stackDepthAround(*func, "compute"), "<nothing>");
    }
} // namespace
