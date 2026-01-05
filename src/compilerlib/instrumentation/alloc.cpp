#include "compilerlib/instrumentation/alloc.hpp"
#include "compilerlib/instrumentation/common.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Casting.h>

namespace compilerlib {
namespace {

llvm::Function *getCalledFunction(llvm::CallBase &call)
{
    if (auto *fn = call.getCalledFunction()) {
        return fn;
    }
    auto *callee = call.getCalledOperand();
    if (!callee) {
        return nullptr;
    }
    callee = callee->stripPointerCasts();
    return llvm::dyn_cast<llvm::Function>(callee);
}

bool isMallocLike(const llvm::Function &fn)
{
    if (!fn.isDeclaration()) {
        return false;
    }
    const auto *fnTy = fn.getFunctionType();
    if (!fnTy || fnTy->isVarArg() || fnTy->getNumParams() != 1) {
        return false;
    }
    if (!fnTy->getReturnType()->isPointerTy()) {
        return false;
    }
    return fnTy->getParamType(0)->isIntegerTy();
}

bool isFreeLike(const llvm::Function &fn)
{
    if (!fn.isDeclaration()) {
        return false;
    }
    const auto *fnTy = fn.getFunctionType();
    if (!fnTy || fnTy->isVarArg() || fnTy->getNumParams() != 1) {
        return false;
    }
    if (!fnTy->getReturnType()->isVoidTy()) {
        return false;
    }
    return fnTy->getParamType(0)->isPointerTy();
}

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

bool isEffectivelyUnused(llvm::Value *value)
{
    llvm::SmallVector<llvm::Value *, 8> worklist;
    llvm::SmallPtrSet<llvm::Value *, 8> visited;

    worklist.push_back(value);
    visited.insert(value);

    while (!worklist.empty()) {
        llvm::Value *current = worklist.pop_back_val();
        for (llvm::Use &use : current->uses()) {
            auto *user = use.getUser();
            if (llvm::isa<llvm::DbgInfoIntrinsic>(user)) {
                continue;
            }
            if (auto *cast = llvm::dyn_cast<llvm::BitCastInst>(user)) {
                if (visited.insert(cast).second) {
                    worklist.push_back(cast);
                }
                continue;
            }
            if (auto *cast = llvm::dyn_cast<llvm::AddrSpaceCastInst>(user)) {
                if (visited.insert(cast).second) {
                    worklist.push_back(cast);
                }
                continue;
            }
            if (auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(user)) {
                if (ce->isCast()) {
                    if (visited.insert(ce).second) {
                        worklist.push_back(ce);
                    }
                    continue;
                }
            }
            return false;
        }
    }

    return true;
}

} // namespace

void wrapAllocCalls(llvm::Module &module)
{
    llvm::LLVMContext &context = module.getContext();
    const llvm::DataLayout &layout = module.getDataLayout();
    llvm::Type *voidPtrTy = llvm::PointerType::get(llvm::Type::getInt8Ty(context), 0);
    llvm::Type *sizeTy = layout.getIntPtrType(context);

    auto *mallocTy = llvm::FunctionType::get(voidPtrTy, {sizeTy, voidPtrTy}, false);
    auto *freeTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {voidPtrTy}, false);

    llvm::FunctionCallee ctMalloc = module.getOrInsertFunction("__ct_malloc", mallocTy);
    llvm::FunctionCallee ctMallocUnreachable = module.getOrInsertFunction("__ct_malloc_unreachable", mallocTy);
    llvm::FunctionCallee ctFree = module.getOrInsertFunction("__ct_free", freeTy);

    llvm::DenseMap<const llvm::DILocation *, llvm::Constant *> siteCache;
    llvm::Constant *unknownSite = nullptr;
    llvm::SmallVector<llvm::CallInst *, 16> mallocCalls;
    llvm::SmallVector<llvm::CallInst *, 16> freeCalls;

    for (llvm::Function &func : module) {
        if (!shouldInstrument(func)) {
            continue;
        }
        for (llvm::BasicBlock &bb : func) {
            for (llvm::Instruction &inst : bb) {
                auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
                if (!call) {
                    continue;
                }

                llvm::Function *callee = getCalledFunction(*call);
                if (!callee) {
                    continue;
                }

                llvm::StringRef name = callee->getName();
                if (name == "malloc") {
                    if (isMallocLike(*callee)) {
                        mallocCalls.push_back(call);
                    }
                } else if (name == "free") {
                    if (isFreeLike(*callee)) {
                        freeCalls.push_back(call);
                    }
                }
            }
        }
    }

    for (llvm::CallInst *call : mallocCalls) {
        llvm::IRBuilder<> builder(call);
        llvm::Value *sizeArg = call->getArgOperand(0);
        if (sizeArg->getType() != sizeTy) {
            sizeArg = builder.CreateZExtOrTrunc(sizeArg, sizeTy);
        }

        llvm::Value *site = getSiteString(module, *call, siteCache, unknownSite);
        llvm::FunctionCallee target = isEffectivelyUnused(call) ? ctMallocUnreachable : ctMalloc;
        auto *newCall = builder.CreateCall(target, {sizeArg, site});
        newCall->setCallingConv(call->getCallingConv());
        newCall->setTailCallKind(call->getTailCallKind());
        newCall->setDebugLoc(call->getDebugLoc());

        call->replaceAllUsesWith(newCall);
        call->eraseFromParent();
    }

    for (llvm::CallInst *call : freeCalls) {
        llvm::IRBuilder<> builder(call);
        llvm::Value *ptrArg = call->getArgOperand(0);
        if (ptrArg->getType() != voidPtrTy) {
            ptrArg = builder.CreateBitCast(ptrArg, voidPtrTy);
        }
        auto *newCall = builder.CreateCall(ctFree, {ptrArg});
        newCall->setCallingConv(call->getCallingConv());
        newCall->setTailCallKind(call->getTailCallKind());
        newCall->setDebugLoc(call->getDebugLoc());
        call->eraseFromParent();
    }
}

} // namespace compilerlib
