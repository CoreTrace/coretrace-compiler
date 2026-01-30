#include "compilerlib/instrumentation/alloc.hpp"
#include "compilerlib/instrumentation/common.hpp"
#include "compilerlib/attributes.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Casting.h>

namespace compilerlib
{
    namespace
    {

        CT_NODISCARD llvm::Function* getCalledFunction(llvm::CallBase& call)
        {
            if (auto* fn = call.getCalledFunction())
            {
                return fn;
            }
            auto* callee = call.getCalledOperand();
            if (!callee)
            {
                return nullptr;
            }
            callee = callee->stripPointerCasts();
            return llvm::dyn_cast<llvm::Function>(callee);
        }

        CT_NODISCARD bool isMallocLike(const llvm::Function& fn)
        {
            if (!fn.isDeclaration())
            {
                return false;
            }
            const auto* fnTy = fn.getFunctionType();
            if (!fnTy || fnTy->isVarArg() || fnTy->getNumParams() != 1)
            {
                return false;
            }
            if (!fnTy->getReturnType()->isPointerTy())
            {
                return false;
            }
            return fnTy->getParamType(0)->isIntegerTy();
        }

        CT_NODISCARD bool isCallocLike(const llvm::Function& fn)
        {
            if (!fn.isDeclaration())
            {
                return false;
            }
            const auto* fnTy = fn.getFunctionType();
            if (!fnTy || fnTy->isVarArg() || fnTy->getNumParams() != 2)
            {
                return false;
            }
            if (!fnTy->getReturnType()->isPointerTy())
            {
                return false;
            }
            return fnTy->getParamType(0)->isIntegerTy() && fnTy->getParamType(1)->isIntegerTy();
        }

        CT_NODISCARD bool isReallocLike(const llvm::Function& fn)
        {
            if (!fn.isDeclaration())
            {
                return false;
            }
            const auto* fnTy = fn.getFunctionType();
            if (!fnTy || fnTy->isVarArg() || fnTy->getNumParams() != 2)
            {
                return false;
            }
            if (!fnTy->getReturnType()->isPointerTy())
            {
                return false;
            }
            return fnTy->getParamType(0)->isPointerTy() && fnTy->getParamType(1)->isIntegerTy();
        }

        CT_NODISCARD bool isFreeLike(const llvm::Function& fn)
        {
            if (!fn.isDeclaration())
            {
                return false;
            }
            const auto* fnTy = fn.getFunctionType();
            if (!fnTy || fnTy->isVarArg() || fnTy->getNumParams() != 1)
            {
                return false;
            }
            if (!fnTy->getReturnType()->isVoidTy())
            {
                return false;
            }
            return fnTy->getParamType(0)->isPointerTy();
        }

        CT_NODISCARD bool isOperatorNewName(llvm::StringRef name, bool& isArray)
        {
            if (name == "_Znwm" || name == "__Znwm")
            {
                isArray = false;
                return true;
            }
            if (name == "_Znam" || name == "__Znam")
            {
                isArray = true;
                return true;
            }
            return false;
        }

        CT_NODISCARD bool isOperatorDeleteName(llvm::StringRef name, bool& isArray)
        {
            if (name == "_ZdlPv" || name == "__ZdlPv")
            {
                isArray = false;
                return true;
            }
            if (name == "_ZdaPv" || name == "__ZdaPv")
            {
                isArray = true;
                return true;
            }
            return false;
        }

        CT_NODISCARD llvm::Constant* createSiteString(llvm::Module& module, llvm::StringRef site)
        {
            llvm::IRBuilder<> builder(module.getContext());
            auto* global = builder.CreateGlobalString(site, ".ct.site", 0, &module);
            auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(module.getContext()), 0);
            llvm::Constant* indices[] = {zero, zero};
            return llvm::ConstantExpr::getInBoundsGetElementPtr(global->getValueType(), global,
                                                                indices);
        }

        CT_NODISCARD llvm::Value*
        getSiteString(llvm::Module& module, const llvm::Instruction& inst,
                      llvm::DenseMap<const llvm::DILocation*, llvm::Constant*>& cache,
                      llvm::Constant*& unknown)
        {
            llvm::DebugLoc loc = inst.getDebugLoc();
            if (!loc)
            {
                if (!unknown)
                {
                    unknown = createSiteString(module, "<unknown>");
                }
                return unknown;
            }

            const llvm::DILocation* di = loc.get();
            if (auto it = cache.find(di); it != cache.end())
            {
                return it->second;
            }

            std::string site = formatSiteString(inst);
            llvm::Constant* value = createSiteString(module, site);
            cache[di] = value;
            return value;
        }

        CT_NODISCARD llvm::CallBase* replaceCall(llvm::CallBase* call, llvm::FunctionCallee target,
                                                 llvm::ArrayRef<llvm::Value*> args)
        {
            if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(call))
            {
                llvm::IRBuilder<> builder(invoke);
                auto* newInvoke = builder.CreateInvoke(target, invoke->getNormalDest(),
                                                       invoke->getUnwindDest(), args);
                newInvoke->setCallingConv(invoke->getCallingConv());
                newInvoke->setDebugLoc(invoke->getDebugLoc());
                invoke->replaceAllUsesWith(newInvoke);
                invoke->eraseFromParent();
                return newInvoke;
            }

            auto* callInst = llvm::cast<llvm::CallInst>(call);
            llvm::IRBuilder<> builder(callInst);
            auto* newCall = builder.CreateCall(target, args);
            newCall->setCallingConv(callInst->getCallingConv());
            newCall->setTailCallKind(callInst->getTailCallKind());
            newCall->setDebugLoc(callInst->getDebugLoc());
            callInst->replaceAllUsesWith(newCall);
            callInst->eraseFromParent();
            return newCall;
        }

        CT_NODISCARD bool isEffectivelyUnused(llvm::Value* value)
        {
            llvm::SmallVector<llvm::Value*, 8> worklist;
            llvm::SmallPtrSet<llvm::Value*, 8> visited;

            worklist.push_back(value);
            visited.insert(value);

            while (!worklist.empty())
            {
                llvm::Value* current = worklist.pop_back_val();
                for (llvm::Use& use : current->uses())
                {
                    auto* user = use.getUser();
                    if (llvm::isa<llvm::DbgInfoIntrinsic>(user))
                    {
                        continue;
                    }
                    if (auto* cast = llvm::dyn_cast<llvm::BitCastInst>(user))
                    {
                        if (visited.insert(cast).second)
                        {
                            worklist.push_back(cast);
                        }
                        continue;
                    }
                    if (auto* cast = llvm::dyn_cast<llvm::AddrSpaceCastInst>(user))
                    {
                        if (visited.insert(cast).second)
                        {
                            worklist.push_back(cast);
                        }
                        continue;
                    }
                    if (auto* ce = llvm::dyn_cast<llvm::ConstantExpr>(user))
                    {
                        if (ce->isCast())
                        {
                            if (visited.insert(ce).second)
                            {
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

    void wrapAllocCalls(llvm::Module& module)
    {
        llvm::LLVMContext& context = module.getContext();
        const llvm::DataLayout& layout = module.getDataLayout();
        llvm::Type* voidPtrTy = llvm::PointerType::get(llvm::Type::getInt8Ty(context), 0);
        llvm::Type* sizeTy = layout.getIntPtrType(context);

        auto* mallocTy = llvm::FunctionType::get(voidPtrTy, {sizeTy, voidPtrTy}, false);
        auto* callocTy = llvm::FunctionType::get(voidPtrTy, {sizeTy, sizeTy, voidPtrTy}, false);
        auto* reallocTy = llvm::FunctionType::get(voidPtrTy, {voidPtrTy, sizeTy, voidPtrTy}, false);
        auto* freeTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {voidPtrTy}, false);

        llvm::FunctionCallee ctMalloc = module.getOrInsertFunction("__ct_malloc", mallocTy);
        llvm::FunctionCallee ctMallocUnreachable =
            module.getOrInsertFunction("__ct_malloc_unreachable", mallocTy);
        llvm::FunctionCallee ctCalloc = module.getOrInsertFunction("__ct_calloc", callocTy);
        llvm::FunctionCallee ctCallocUnreachable =
            module.getOrInsertFunction("__ct_calloc_unreachable", callocTy);
        llvm::FunctionCallee ctRealloc = module.getOrInsertFunction("__ct_realloc", reallocTy);
        llvm::FunctionCallee ctNew = module.getOrInsertFunction("__ct_new", mallocTy);
        llvm::FunctionCallee ctNewUnreachable =
            module.getOrInsertFunction("__ct_new_unreachable", mallocTy);
        llvm::FunctionCallee ctNewArray = module.getOrInsertFunction("__ct_new_array", mallocTy);
        llvm::FunctionCallee ctNewArrayUnreachable =
            module.getOrInsertFunction("__ct_new_array_unreachable", mallocTy);
        llvm::FunctionCallee ctFree = module.getOrInsertFunction("__ct_free", freeTy);
        llvm::FunctionCallee ctDelete = module.getOrInsertFunction("__ct_delete", freeTy);
        llvm::FunctionCallee ctDeleteArray =
            module.getOrInsertFunction("__ct_delete_array", freeTy);

        llvm::DenseMap<const llvm::DILocation*, llvm::Constant*> siteCache;
        llvm::Constant* unknownSite = nullptr;
        llvm::SmallVector<llvm::CallBase*, 16> mallocCalls;
        llvm::SmallVector<llvm::CallBase*, 16> callocCalls;
        llvm::SmallVector<llvm::CallBase*, 16> reallocCalls;
        llvm::SmallVector<llvm::CallBase*, 16> freeCalls;
        llvm::SmallVector<llvm::CallBase*, 16> newCalls;
        llvm::SmallVector<llvm::CallBase*, 16> newArrayCalls;
        llvm::SmallVector<llvm::CallBase*, 16> deleteCalls;
        llvm::SmallVector<llvm::CallBase*, 16> deleteArrayCalls;

        for (llvm::Function& func : module)
        {
            if (!shouldInstrument(func))
            {
                continue;
            }
            for (llvm::BasicBlock& bb : func)
            {
                for (llvm::Instruction& inst : bb)
                {
                    auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                    if (!call)
                    {
                        continue;
                    }

                    llvm::Function* callee = getCalledFunction(*call);
                    if (!callee)
                    {
                        continue;
                    }

                    llvm::StringRef name = callee->getName();
                    if (name == "malloc")
                    {
                        if (isMallocLike(*callee))
                        {
                            mallocCalls.push_back(call);
                        }
                        continue;
                    }
                    if (name == "calloc")
                    {
                        if (isCallocLike(*callee))
                        {
                            callocCalls.push_back(call);
                        }
                        continue;
                    }
                    if (name == "realloc")
                    {
                        if (isReallocLike(*callee))
                        {
                            reallocCalls.push_back(call);
                        }
                        continue;
                    }
                    if (name == "free")
                    {
                        if (isFreeLike(*callee))
                        {
                            freeCalls.push_back(call);
                        }
                        continue;
                    }

                    bool isArray = false;
                    if (isOperatorNewName(name, isArray) && isMallocLike(*callee))
                    {
                        if (isArray)
                        {
                            newArrayCalls.push_back(call);
                        }
                        else
                        {
                            newCalls.push_back(call);
                        }
                        continue;
                    }
                    if (isOperatorDeleteName(name, isArray) && isFreeLike(*callee))
                    {
                        if (isArray)
                        {
                            deleteArrayCalls.push_back(call);
                        }
                        else
                        {
                            deleteCalls.push_back(call);
                        }
                    }
                }
            }
        }

        for (llvm::CallBase* call : mallocCalls)
        {
            llvm::IRBuilder<> builder(call);
            llvm::Value* sizeArg = call->getArgOperand(0);
            if (sizeArg->getType() != sizeTy)
            {
                sizeArg = builder.CreateZExtOrTrunc(sizeArg, sizeTy);
            }

            llvm::Value* site = getSiteString(module, *call, siteCache, unknownSite);
            llvm::FunctionCallee target =
                isEffectivelyUnused(call) ? ctMallocUnreachable : ctMalloc;
            (void)replaceCall(call, target, {sizeArg, site});
        }

        for (llvm::CallBase* call : callocCalls)
        {
            llvm::IRBuilder<> builder(call);
            llvm::Value* countArg = call->getArgOperand(0);
            llvm::Value* sizeArg = call->getArgOperand(1);
            if (countArg->getType() != sizeTy)
            {
                countArg = builder.CreateZExtOrTrunc(countArg, sizeTy);
            }
            if (sizeArg->getType() != sizeTy)
            {
                sizeArg = builder.CreateZExtOrTrunc(sizeArg, sizeTy);
            }

            llvm::Value* site = getSiteString(module, *call, siteCache, unknownSite);
            llvm::FunctionCallee target =
                isEffectivelyUnused(call) ? ctCallocUnreachable : ctCalloc;
            (void)replaceCall(call, target, {countArg, sizeArg, site});
        }

        for (llvm::CallBase* call : reallocCalls)
        {
            llvm::IRBuilder<> builder(call);
            llvm::Value* ptrArg = call->getArgOperand(0);
            llvm::Value* sizeArg = call->getArgOperand(1);
            if (ptrArg->getType() != voidPtrTy)
            {
                ptrArg = builder.CreateBitCast(ptrArg, voidPtrTy);
            }
            if (sizeArg->getType() != sizeTy)
            {
                sizeArg = builder.CreateZExtOrTrunc(sizeArg, sizeTy);
            }

            llvm::Value* site = getSiteString(module, *call, siteCache, unknownSite);
            (void)replaceCall(call, ctRealloc, {ptrArg, sizeArg, site});
        }

        for (llvm::CallBase* call : newCalls)
        {
            llvm::IRBuilder<> builder(call);
            llvm::Value* sizeArg = call->getArgOperand(0);
            if (sizeArg->getType() != sizeTy)
            {
                sizeArg = builder.CreateZExtOrTrunc(sizeArg, sizeTy);
            }
            llvm::Value* site = getSiteString(module, *call, siteCache, unknownSite);
            llvm::FunctionCallee target = isEffectivelyUnused(call) ? ctNewUnreachable : ctNew;
            (void)replaceCall(call, target, {sizeArg, site});
        }

        for (llvm::CallBase* call : newArrayCalls)
        {
            llvm::IRBuilder<> builder(call);
            llvm::Value* sizeArg = call->getArgOperand(0);
            if (sizeArg->getType() != sizeTy)
            {
                sizeArg = builder.CreateZExtOrTrunc(sizeArg, sizeTy);
            }
            llvm::Value* site = getSiteString(module, *call, siteCache, unknownSite);
            llvm::FunctionCallee target =
                isEffectivelyUnused(call) ? ctNewArrayUnreachable : ctNewArray;
            (void)replaceCall(call, target, {sizeArg, site});
        }

        for (llvm::CallBase* call : freeCalls)
        {
            llvm::IRBuilder<> builder(call);
            llvm::Value* ptrArg = call->getArgOperand(0);
            if (ptrArg->getType() != voidPtrTy)
            {
                ptrArg = builder.CreateBitCast(ptrArg, voidPtrTy);
            }
            (void)replaceCall(call, ctFree, {ptrArg});
        }

        for (llvm::CallBase* call : deleteCalls)
        {
            llvm::IRBuilder<> builder(call);
            llvm::Value* ptrArg = call->getArgOperand(0);
            if (ptrArg->getType() != voidPtrTy)
            {
                ptrArg = builder.CreateBitCast(ptrArg, voidPtrTy);
            }
            (void)replaceCall(call, ctDelete, {ptrArg});
        }

        for (llvm::CallBase* call : deleteArrayCalls)
        {
            llvm::IRBuilder<> builder(call);
            llvm::Value* ptrArg = call->getArgOperand(0);
            if (ptrArg->getType() != voidPtrTy)
            {
                ptrArg = builder.CreateBitCast(ptrArg, voidPtrTy);
            }
            (void)replaceCall(call, ctDeleteArray, {ptrArg});
        }
    }

} // namespace compilerlib
