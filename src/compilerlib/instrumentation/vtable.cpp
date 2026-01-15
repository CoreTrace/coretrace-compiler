#include "compilerlib/instrumentation/vtable.hpp"
#include "compilerlib/instrumentation/common.hpp"

#include <llvm/Config/llvm-config.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Casting.h>

namespace compilerlib {
namespace {

llvm::Constant *createSiteString(llvm::Module &module, llvm::StringRef site)
{
    llvm::IRBuilder<> builder(module.getContext());
    auto *global = builder.CreateGlobalString(site, ".ct.site", 0, &module);
    auto *zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(module.getContext()), 0);
    llvm::Constant *indices[] = {zero, zero};
    return llvm::ConstantExpr::getInBoundsGetElementPtr(global->getValueType(), global, indices);
}

llvm::Value *getSiteString(llvm::Module &module,
                           const llvm::Instruction &inst,
                           llvm::DenseMap<const llvm::DILocation *, llvm::Constant *> &cache,
                           llvm::Constant *&unknown)
{
    llvm::DebugLoc loc = inst.getDebugLoc();
    if (!loc) {
        if (!unknown) {
            unknown = createSiteString(module, "<unknown>");
        }
        return unknown;
    }

    const llvm::DILocation *di = loc.get();
    if (auto it = cache.find(di); it != cache.end()) {
        return it->second;
    }

    std::string site = formatSiteString(inst);
    llvm::Constant *value = createSiteString(module, site);
    cache[di] = value;
    return value;
}

llvm::Value *stripPointerCasts(llvm::Value *value)
{
    if (!value) {
        return nullptr;
    }
    return value->stripPointerCasts();
}

llvm::Value *findThisPointerFromCallTarget(llvm::Value *called)
{
    auto *load = llvm::dyn_cast_or_null<llvm::LoadInst>(stripPointerCasts(called));
    if (!load) {
        return nullptr;
    }

    llvm::Value *ptr = stripPointerCasts(load->getPointerOperand());
    if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(ptr)) {
        ptr = stripPointerCasts(gep->getPointerOperand());
    }

    auto *vptrLoad = llvm::dyn_cast_or_null<llvm::LoadInst>(stripPointerCasts(ptr));
    if (!vptrLoad) {
        return nullptr;
    }

    return stripPointerCasts(vptrLoad->getPointerOperand());
}

bool shouldTraceCall(const llvm::CallBase &call)
{
    if (call.isInlineAsm()) {
        return false;
    }
    if (call.getCalledFunction() != nullptr) {
        return false;
    }
    return true;
}

#if LLVM_VERSION_MAJOR < 17
llvm::StringRef stripTypePrefix(llvm::StringRef name)
{
    if (name.starts_with("class.")) {
        return name.drop_front(6);
    }
    if (name.starts_with("struct.")) {
        return name.drop_front(7);
    }
    if (name.starts_with("union.")) {
        return name.drop_front(6);
    }
    return name;
}
#endif

llvm::Value *getStaticTypeString(llvm::Module &module,
                                 llvm::Value *thisPtr,
                                 llvm::StringMap<llvm::Constant *> &cache,
                                 llvm::Constant *&unknown)
{
    if (!thisPtr) {
        if (!unknown) {
            unknown = createSiteString(module, "<unknown>");
        }
        return unknown;
    }

    std::string typeName;
#if LLVM_VERSION_MAJOR < 17
    if (auto *ptrTy = llvm::dyn_cast<llvm::PointerType>(thisPtr->getType())) {
        llvm::Type *elemTy = ptrTy->getPointerElementType();
        if (auto *structTy = llvm::dyn_cast<llvm::StructType>(elemTy)) {
            if (structTy->hasName()) {
                llvm::StringRef name = stripTypePrefix(structTy->getName());
                typeName = name.str();
            }
        }
    }
#endif

    if (typeName.empty()) {
        if (!unknown) {
            unknown = createSiteString(module, "<unknown>");
        }
        return unknown;
    }

    if (auto it = cache.find(typeName); it != cache.end()) {
        return it->second;
    }

    llvm::Constant *value = createSiteString(module, typeName);
    cache.try_emplace(typeName, value);
    return value;
}

} // namespace

void instrumentVirtualCalls(llvm::Module &module, bool trace_calls, bool dump_vtable)
{
    if (!trace_calls && !dump_vtable) {
        return;
    }

    llvm::LLVMContext &context = module.getContext();
    llvm::Type *voidTy = llvm::Type::getVoidTy(context);
    llvm::Type *voidPtrTy = llvm::PointerType::get(llvm::Type::getInt8Ty(context), 0);

    auto *traceTy = llvm::FunctionType::get(voidTy, {voidPtrTy, voidPtrTy, voidPtrTy, voidPtrTy}, false);
    auto *dumpTy = llvm::FunctionType::get(voidTy, {voidPtrTy, voidPtrTy, voidPtrTy}, false);

    llvm::FunctionCallee traceFn = module.getOrInsertFunction("__ct_vcall_trace", traceTy);
    llvm::FunctionCallee dumpFn = module.getOrInsertFunction("__ct_vtable_dump", dumpTy);

    llvm::DenseMap<const llvm::DILocation *, llvm::Constant *> siteCache;
    llvm::Constant *unknownSite = nullptr;
    llvm::StringMap<llvm::Constant *> typeCache;
    llvm::Constant *unknownType = nullptr;
    llvm::SmallVector<llvm::CallBase *, 64> worklist;

    for (llvm::Function &func : module) {
        if (!shouldInstrument(func)) {
            continue;
        }
        for (llvm::BasicBlock &bb : func) {
            for (llvm::Instruction &inst : bb) {
                auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
                if (!call) {
                    continue;
                }
                if (!shouldTraceCall(*call)) {
                    continue;
                }
                worklist.push_back(call);
            }
        }
    }

    for (llvm::CallBase *call : worklist) {
        llvm::Value *thisPtr = findThisPointerFromCallTarget(call->getCalledOperand());
        if (!thisPtr || !thisPtr->getType()->isPointerTy()) {
            continue;
        }

        llvm::IRBuilder<> builder(call);
        llvm::Value *site = getSiteString(module, *call, siteCache, unknownSite);
        llvm::Value *staticType = getStaticTypeString(module, thisPtr, typeCache, unknownType);

        llvm::Value *thisCast = thisPtr;
        if (thisCast->getType() != voidPtrTy) {
            thisCast = builder.CreateBitCast(thisCast, voidPtrTy);
        }

        if (dump_vtable) {
            builder.CreateCall(dumpFn, {thisCast, site, staticType});
        }

        if (trace_calls) {
            llvm::Value *callee = call->getCalledOperand();
            llvm::Value *calleeCast = callee;
            if (calleeCast->getType() != voidPtrTy) {
                calleeCast = builder.CreateBitCast(calleeCast, voidPtrTy);
            }
            builder.CreateCall(traceFn, {thisCast, calleeCast, site, staticType});
        }
    }
}

} // namespace compilerlib
