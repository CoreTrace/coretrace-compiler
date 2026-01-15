#include "compilerlib/instrumentation/trace.hpp"
#include "compilerlib/instrumentation/common.hpp"

#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Casting.h>

namespace compilerlib
{
namespace
{

llvm::Value *getStringLiteral(llvm::Module &module,
                              llvm::StringRef text,
                              llvm::StringMap<llvm::Constant *> &cache)
{
    if (auto it = cache.find(text); it != cache.end()) {
        return it->second;
    }

    llvm::IRBuilder<> builder(module.getContext());
    auto *global = builder.CreateGlobalString(text, ".ct.func", 0, &module);
    auto *zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(module.getContext()), 0);
    llvm::Constant *indices[] = {zero, zero};
    llvm::Constant *value =
        llvm::ConstantExpr::getInBoundsGetElementPtr(global->getValueType(), global, indices);
    cache.try_emplace(text, value);
    return value;
}

} // namespace

void instrumentModule(llvm::Module &module)
{
    llvm::LLVMContext &context = module.getContext();
    llvm::Type *voidTy = llvm::Type::getVoidTy(context);
    llvm::Type *voidPtrTy = llvm::PointerType::get(llvm::Type::getInt8Ty(context), 0);
    llvm::Type *i64Ty = llvm::Type::getInt64Ty(context);
    llvm::Type *doubleTy = llvm::Type::getDoubleTy(context);

    auto *enterTy = llvm::FunctionType::get(voidTy, {voidPtrTy}, false);
    auto *exitVoidTy = llvm::FunctionType::get(voidTy, {voidPtrTy}, false);
    auto *exitI64Ty = llvm::FunctionType::get(voidTy, {voidPtrTy, i64Ty}, false);
    auto *exitPtrTy = llvm::FunctionType::get(voidTy, {voidPtrTy, voidPtrTy}, false);
    auto *exitF64Ty = llvm::FunctionType::get(voidTy, {voidPtrTy, doubleTy}, false);
    auto *exitUnknownTy = llvm::FunctionType::get(voidTy, {voidPtrTy}, false);

    llvm::FunctionCallee enterFn = module.getOrInsertFunction("__ct_trace_enter", enterTy);
    llvm::FunctionCallee exitVoidFn = module.getOrInsertFunction("__ct_trace_exit_void", exitVoidTy);
    llvm::FunctionCallee exitI64Fn = module.getOrInsertFunction("__ct_trace_exit_i64", exitI64Ty);
    llvm::FunctionCallee exitPtrFn = module.getOrInsertFunction("__ct_trace_exit_ptr", exitPtrTy);
    llvm::FunctionCallee exitF64Fn = module.getOrInsertFunction("__ct_trace_exit_f64", exitF64Ty);
    llvm::FunctionCallee exitUnknownFn = module.getOrInsertFunction("__ct_trace_exit_unknown", exitUnknownTy);

    llvm::StringMap<llvm::Constant *> funcNameCache;
    for (llvm::Function &func : module) {
        if (!shouldInstrument(func)) {
            continue;
        }

        llvm::BasicBlock &entry = func.getEntryBlock();
        llvm::IRBuilder<> entryBuilder(&*entry.getFirstInsertionPt());
        llvm::Value *funcName = getStringLiteral(module, func.getName(), funcNameCache);
        entryBuilder.CreateCall(enterFn, {funcName});

        llvm::SmallVector<llvm::ReturnInst *, 8> returns;
        for (llvm::BasicBlock &bb : func) {
            if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator())) {
                returns.push_back(ret);
            }
        }

        llvm::Type *retTy = func.getReturnType();
        for (llvm::ReturnInst *ret : returns) {
            llvm::IRBuilder<> retBuilder(ret);
            if (retTy->isVoidTy()) {
                retBuilder.CreateCall(exitVoidFn, {funcName});
                continue;
            }

            llvm::Value *retVal = ret->getReturnValue();
            if (!retVal) {
                retBuilder.CreateCall(exitUnknownFn, {funcName});
                continue;
            }

            if (retTy->isIntegerTy() && retTy->getIntegerBitWidth() <= 64) {
                if (retVal->getType() != i64Ty) {
                    retVal = retBuilder.CreateSExtOrTrunc(retVal, i64Ty);
                }
                retBuilder.CreateCall(exitI64Fn, {funcName, retVal});
                continue;
            }

            if (retTy->isPointerTy()) {
                if (retVal->getType() != voidPtrTy) {
                    retVal = retBuilder.CreateBitCast(retVal, voidPtrTy);
                }
                retBuilder.CreateCall(exitPtrFn, {funcName, retVal});
                continue;
            }

            if (retTy->isFloatingPointTy()) {
                if (retVal->getType() != doubleTy) {
                    retVal = retBuilder.CreateFPExt(retVal, doubleTy);
                }
                retBuilder.CreateCall(exitF64Fn, {funcName, retVal});
                continue;
            }

            retBuilder.CreateCall(exitUnknownFn, {funcName});
        }
    }
}

} // namespace compilerlib
