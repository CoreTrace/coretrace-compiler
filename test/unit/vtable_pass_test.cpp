// SPDX-License-Identifier: Apache-2.0
// Runs the vtable pass on hand-written IR and checks which indirect calls it takes for
// virtual calls, and the runtime calls it puts before them.
#include "compilerlib/instrumentation/vtable.hpp"

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
    // A virtual call as clang emits it: the vtable pointer is loaded from the object, the
    // function from a slot of the vtable, and the object is passed as `this`.
    constexpr const char* kVirtualCall = R"(
define void @f(ptr %object) {
  %vtable = load ptr, ptr %object
  %slot = getelementptr inbounds ptr, ptr %vtable, i64 2
  %target = load ptr, ptr %slot
  call void %target(ptr %object)
  ret void
}
)";

    class VtablePassTest : public ::testing::Test
    {
      protected:
        // Parses `ir`, runs the vtable pass on it and requires valid IR. Returns @f.
        llvm::Function* instrument(const char* ir, bool trace, bool dump)
        {
            llvm::SMDiagnostic parseError;
            module_ = llvm::parseAssemblyString(ir, parseError, context_);
            if (!module_)
            {
                ADD_FAILURE() << parseError.getMessage().str();
                return nullptr;
            }
            compilerlib::instrumentVirtualCalls(*module_, trace, dump);

            std::string problems;
            llvm::raw_string_ostream stream(problems);
            EXPECT_FALSE(llvm::verifyModule(*module_, &stream)) << stream.str();
            return module_->getFunction("f");
        }

        // The runtime functions @f calls, in order.
        static std::vector<std::string> runtimeCalls(llvm::Function& func)
        {
            std::vector<std::string> names;
            for (llvm::Instruction& inst : llvm::instructions(func))
            {
                auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                llvm::Function* callee = call ? call->getCalledFunction() : nullptr;
                if (callee && callee->getName().starts_with("__ct_"))
                {
                    names.push_back(callee->getName().str());
                }
            }
            return names;
        }

        // The call to runtime function `name` in @f; null when there is none.
        static llvm::CallBase* runtimeCall(llvm::Function& func, llvm::StringRef name)
        {
            for (llvm::Instruction& inst : llvm::instructions(func))
            {
                auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                if (call && call->getCalledFunction() &&
                    call->getCalledFunction()->getName() == name)
                {
                    return call;
                }
            }
            return nullptr;
        }

        static std::string operandName(const llvm::CallBase& call, unsigned index)
        {
            const llvm::Value* operand = call.getArgOperand(index);
            return operand->hasName() ? operand->getName().str() : "<unnamed value>";
        }

        llvm::LLVMContext context_;
        std::unique_ptr<llvm::Module> module_;
    };

    TEST_F(VtablePassTest, VirtualCallIsTracedWithItsObjectAndTarget)
    {
        llvm::Function* func = instrument(kVirtualCall, /*trace=*/true, /*dump=*/false);
        ASSERT_NE(func, nullptr);
        EXPECT_EQ(runtimeCalls(*func), std::vector<std::string>{"__ct_vcall_trace"});
        llvm::CallBase* trace = runtimeCall(*func, "__ct_vcall_trace");
        ASSERT_NE(trace, nullptr);
        EXPECT_EQ(operandName(*trace, 0), "object");
        EXPECT_EQ(operandName(*trace, 1), "target");
        // Right before the virtual call.
        auto* next = llvm::dyn_cast<llvm::CallBase>(trace->getNextNode());
        ASSERT_NE(next, nullptr);
        EXPECT_EQ(next->getCalledOperand()->getName(), "target");
    }

    TEST_F(VtablePassTest, VtableOfTheObjectIsDumpedBeforeTheCall)
    {
        llvm::Function* func = instrument(kVirtualCall, /*trace=*/false, /*dump=*/true);
        ASSERT_NE(func, nullptr);
        EXPECT_EQ(runtimeCalls(*func), std::vector<std::string>{"__ct_vtable_dump"});
        llvm::CallBase* dump = runtimeCall(*func, "__ct_vtable_dump");
        ASSERT_NE(dump, nullptr);
        EXPECT_EQ(operandName(*dump, 0), "object");
    }

    TEST_F(VtablePassTest, DumpComesBeforeTheTrace)
    {
        llvm::Function* func = instrument(kVirtualCall, /*trace=*/true, /*dump=*/true);
        ASSERT_NE(func, nullptr);
        EXPECT_EQ(runtimeCalls(*func),
                  (std::vector<std::string>{"__ct_vtable_dump", "__ct_vcall_trace"}));
    }

    TEST_F(VtablePassTest, NothingIsDeclaredWhenBothAreDisabled)
    {
        llvm::Function* func = instrument(kVirtualCall, /*trace=*/false, /*dump=*/false);
        ASSERT_NE(func, nullptr);
        EXPECT_TRUE(runtimeCalls(*func).empty());
        EXPECT_EQ(module_->getFunction("__ct_vcall_trace"), nullptr);
        EXPECT_EQ(module_->getFunction("__ct_vtable_dump"), nullptr);
    }

    // The first slot of the vtable is loaded without an index.
    TEST_F(VtablePassTest, CallThroughTheFirstSlotIsAVirtualCall)
    {
        llvm::Function* func = instrument(R"(
define void @f(ptr %object) {
  %vtable = load ptr, ptr %object
  %target = load ptr, ptr %vtable
  call void %target(ptr %object)
  ret void
}
)",
                                          /*trace=*/true, /*dump=*/false);
        ASSERT_NE(func, nullptr);
        llvm::CallBase* trace = runtimeCall(*func, "__ct_vcall_trace");
        ASSERT_NE(trace, nullptr);
        EXPECT_EQ(operandName(*trace, 0), "object");
    }

    // A virtual call that may throw, in a function with cleanups.
    TEST_F(VtablePassTest, VirtualInvokeIsTracedBeforeIt)
    {
        llvm::Function* func = instrument(R"(
declare i32 @__gxx_personality_v0(...)

define void @f(ptr %object) personality ptr @__gxx_personality_v0 {
  %vtable = load ptr, ptr %object
  %slot = getelementptr inbounds ptr, ptr %vtable, i64 1
  %target = load ptr, ptr %slot
  invoke void %target(ptr %object) to label %done unwind label %cleanup

done:
  ret void

cleanup:
  %caught = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %caught
}
)",
                                          /*trace=*/true, /*dump=*/false);
        ASSERT_NE(func, nullptr);
        llvm::CallBase* trace = runtimeCall(*func, "__ct_vcall_trace");
        ASSERT_NE(trace, nullptr);
        EXPECT_TRUE(llvm::isa<llvm::InvokeInst>(trace->getNextNode()));
    }

    TEST_F(VtablePassTest, CallThroughAFunctionPointerArgumentIsNotAVirtualCall)
    {
        llvm::Function* func = instrument(R"(
define void @f(ptr %callback, ptr %object) {
  call void %callback(ptr %object)
  ret void
}
)",
                                          /*trace=*/true, /*dump=*/true);
        ASSERT_NE(func, nullptr);
        EXPECT_TRUE(runtimeCalls(*func).empty());
    }

    TEST_F(VtablePassTest, DirectCallIsNotAVirtualCall)
    {
        llvm::Function* func = instrument(R"(
declare void @method(ptr)

define void @f(ptr %object) {
  call void @method(ptr %object)
  ret void
}
)",
                                          /*trace=*/true, /*dump=*/true);
        ASSERT_NE(func, nullptr);
        EXPECT_TRUE(runtimeCalls(*func).empty());
    }

    // Inline functions and template instantiations, which every unit may emit, are left
    // alone.
    TEST_F(VtablePassTest, LinkOnceFunctionIsLeftAlone)
    {
        llvm::Function* func = instrument(R"(
define linkonce_odr void @f(ptr %object) {
  %vtable = load ptr, ptr %object
  %target = load ptr, ptr %vtable
  call void %target(ptr %object)
  ret void
}
)",
                                          /*trace=*/true, /*dump=*/true);
        ASSERT_NE(func, nullptr);
        EXPECT_TRUE(runtimeCalls(*func).empty());
    }
} // namespace
