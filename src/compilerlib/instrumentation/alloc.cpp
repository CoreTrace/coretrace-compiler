#include "compilerlib/instrumentation/alloc.hpp"
#include "compilerlib/instrumentation/common.hpp"
#include "compilerlib/attributes.hpp"

#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>

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

        CT_NODISCARD bool isNewLike(const llvm::Function& fn)
        {
            if (!fn.isDeclaration())
            {
                return false;
            }
            const auto* fnTy = fn.getFunctionType();
            if (!fnTy || fnTy->isVarArg() || fnTy->getNumParams() < 1)
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

        CT_NODISCARD bool isDeleteLike(const llvm::Function& fn)
        {
            if (!fn.isDeclaration())
            {
                return false;
            }
            const auto* fnTy = fn.getFunctionType();
            if (!fnTy || fnTy->isVarArg() || fnTy->getNumParams() < 1)
            {
                return false;
            }
            if (!fnTy->getReturnType()->isVoidTy())
            {
                return false;
            }
            return fnTy->getParamType(0)->isPointerTy();
        }

        enum class OperatorNewKind
        {
            Normal,
            Nothrow
        };

        enum class OperatorDeleteKind
        {
            Normal,
            Nothrow,
            Destroying
        };

        enum class ReturnAllocKind
        {
            None,
            MallocLike,
            NewLike,
            NewArrayLike,
            MmapLike,
            SbrkLike
        };

        enum class EscapeState
        {
            Unreachable,
            ReachableLocal,
            ReachableGlobal,
            EscapedReturn,
            EscapedCall,
            EscapedStore,
            EscapedScan
        };

        CT_NODISCARD int escapeRank(EscapeState state)
        {
            switch (state)
            {
            case EscapeState::Unreachable:
                return 0;
            case EscapeState::ReachableLocal:
                return 1;
            case EscapeState::ReachableGlobal:
                return 2;
            case EscapeState::EscapedStore:
                return 3;
            case EscapeState::EscapedCall:
                return 4;
            case EscapeState::EscapedReturn:
                return 5;
            case EscapeState::EscapedScan:
                return 6;
            }
            return 6;
        }

        CT_NODISCARD const char* escapeStateName(EscapeState state)
        {
            switch (state)
            {
            case EscapeState::Unreachable:
                return "UNREACHABLE";
            case EscapeState::ReachableLocal:
                return "REACHABLE_LOCAL";
            case EscapeState::ReachableGlobal:
                return "REACHABLE_GLOBAL";
            case EscapeState::EscapedReturn:
                return "ESCAPED_RETURN";
            case EscapeState::EscapedCall:
                return "ESCAPED_CALL";
            case EscapeState::EscapedStore:
                return "ESCAPED_STORE";
            case EscapeState::EscapedScan:
                return "ESCAPED_SCAN";
            }
            return "UNKNOWN";
        }

        CT_NODISCARD bool isAlignedAllocLike(const llvm::Function& fn);
        CT_NODISCARD llvm::StringRef normalizeSymbolName(llvm::StringRef name)
        {
            // LLVM uses the '\01' prefix to mark asm labels (e.g. @"\01_mmap").
            if (!name.empty() && name.front() == '\01')
            {
                return name.drop_front();
            }
            return name;
        }

        CT_NODISCARD bool isMmapLikeName(llvm::StringRef name)
        {
            name = normalizeSymbolName(name);
            return name == "mmap" || name == "_mmap" || name == "__mmap" ||
                   name.starts_with("mmap$") || name.starts_with("_mmap$") ||
                   name.starts_with("__mmap$");
        }
        CT_NODISCARD bool isMunmapLikeName(llvm::StringRef name)
        {
            name = normalizeSymbolName(name);
            return name == "munmap" || name == "_munmap" || name == "__munmap" ||
                   name.starts_with("munmap$") || name.starts_with("_munmap$") ||
                   name.starts_with("__munmap$");
        }
        CT_NODISCARD bool isBrkLikeName(llvm::StringRef name)
        {
            name = normalizeSymbolName(name);
            return name == "brk" || name == "_brk" || name == "__brk" || name.starts_with("brk$") ||
                   name.starts_with("_brk$") || name.starts_with("__brk$");
        }
        CT_NODISCARD bool isSbrkLikeName(llvm::StringRef name)
        {
            name = normalizeSymbolName(name);
            return name == "sbrk" || name == "_sbrk" || name == "__sbrk" ||
                   name.starts_with("sbrk$") || name.starts_with("_sbrk$") ||
                   name.starts_with("__sbrk$");
        }

        CT_NODISCARD bool isOperatorNewName(llvm::StringRef name, bool& isArray,
                                            OperatorNewKind& kind)
        {
            // Itanium C++ ABI manglings:
            // new(size_t):                _Znwm
            // new[](size_t):              _Znam
            // nothrow new:                _ZnwmRKSt9nothrow_t / _ZnamRKSt9nothrow_t
            if (name == "_Znwm" || name == "__Znwm")
            {
                isArray = false;
                kind = OperatorNewKind::Normal;
                return true;
            }
            if (name == "_Znam" || name == "__Znam")
            {
                isArray = true;
                kind = OperatorNewKind::Normal;
                return true;
            }
            if (name == "_ZnwmRKSt9nothrow_t" || name == "__ZnwmRKSt9nothrow_t")
            {
                isArray = false;
                kind = OperatorNewKind::Nothrow;
                return true;
            }
            if (name == "_ZnamRKSt9nothrow_t" || name == "__ZnamRKSt9nothrow_t")
            {
                isArray = true;
                kind = OperatorNewKind::Nothrow;
                return true;
            }
            return false;
        }

        CT_NODISCARD bool isOperatorDeleteName(llvm::StringRef name, bool& isArray,
                                               OperatorDeleteKind& kind)
        {
            // Itanium C++ ABI manglings:
            // delete(void*):               _ZdlPv
            // delete[](void*):             _ZdaPv
            // sized delete:                _ZdlPvm / _ZdaPvm
            // aligned delete:              _ZdlPvSt11align_val_t / _ZdaPvSt11align_val_t
            // sized + aligned delete:      _ZdlPvmSt11align_val_t / _ZdaPvmSt11align_val_t
            // nothrow delete:              _ZdlPvRKSt9nothrow_t / _ZdaPvRKSt9nothrow_t
            // aligned + nothrow delete:    _ZdlPvSt11align_val_tRKSt9nothrow_t / _ZdaPvSt11align_val_tRKSt9nothrow_t
            // sized + nothrow delete:      _ZdlPvmRKSt9nothrow_t / _ZdaPvmRKSt9nothrow_t
            // sized + aligned + nothrow:   _ZdlPvmSt11align_val_tRKSt9nothrow_t / _ZdaPvmSt11align_val_tRKSt9nothrow_t
            // destroying delete (C++20):  _ZdlPvSt19destroying_delete_t
            // Some toolchains prefix an extra underscore.
            if (name == "_ZdlPv" || name == "__ZdlPv" || name == "_ZdlPvm" || name == "__ZdlPvm" ||
                name == "_ZdlPvSt11align_val_t" || name == "__ZdlPvSt11align_val_t" ||
                name == "_ZdlPvmSt11align_val_t" || name == "__ZdlPvmSt11align_val_t" ||
                name == "_ZdlPvRKSt9nothrow_t" || name == "__ZdlPvRKSt9nothrow_t" ||
                name == "_ZdlPvSt11align_val_tRKSt9nothrow_t" ||
                name == "__ZdlPvSt11align_val_tRKSt9nothrow_t" || name == "_ZdlPvmRKSt9nothrow_t" ||
                name == "__ZdlPvmRKSt9nothrow_t" ||
                name == "_ZdlPvmSt11align_val_tRKSt9nothrow_t" ||
                name == "__ZdlPvmSt11align_val_tRKSt9nothrow_t" ||
                name == "_ZdlPvSt19destroying_delete_t" || name == "__ZdlPvSt19destroying_delete_t")
            {
                isArray = false;
                if (name.contains("destroying_delete_t"))
                {
                    kind = OperatorDeleteKind::Destroying;
                }
                else if (name.contains("nothrow_t"))
                {
                    kind = OperatorDeleteKind::Nothrow;
                }
                else
                {
                    kind = OperatorDeleteKind::Normal;
                }
                return true;
            }
            if (name == "_ZdaPv" || name == "__ZdaPv" || name == "_ZdaPvm" || name == "__ZdaPvm" ||
                name == "_ZdaPvSt11align_val_t" || name == "__ZdaPvSt11align_val_t" ||
                name == "_ZdaPvmSt11align_val_t" || name == "__ZdaPvmSt11align_val_t" ||
                name == "_ZdaPvRKSt9nothrow_t" || name == "__ZdaPvRKSt9nothrow_t" ||
                name == "_ZdaPvSt11align_val_tRKSt9nothrow_t" ||
                name == "__ZdaPvSt11align_val_tRKSt9nothrow_t" || name == "_ZdaPvmRKSt9nothrow_t" ||
                name == "__ZdaPvmRKSt9nothrow_t" ||
                name == "_ZdaPvmSt11align_val_tRKSt9nothrow_t" ||
                name == "__ZdaPvmSt11align_val_tRKSt9nothrow_t" ||
                name == "_ZdaPvSt19destroying_delete_t" || name == "__ZdaPvSt19destroying_delete_t")
            {
                isArray = true;
                if (name.contains("destroying_delete_t"))
                {
                    kind = OperatorDeleteKind::Destroying;
                }
                else if (name.contains("nothrow_t"))
                {
                    kind = OperatorDeleteKind::Nothrow;
                }
                else
                {
                    kind = OperatorDeleteKind::Normal;
                }
                return true;
            }
            return false;
        }

        CT_NODISCARD ReturnAllocKind classifyAllocatorCallee(llvm::Function* callee)
        {
            if (!callee)
            {
                return ReturnAllocKind::None;
            }
            llvm::StringRef name = callee->getName();
            if ((name == "malloc" || name == "calloc" || name == "aligned_alloc") &&
                (isMallocLike(*callee) || isCallocLike(*callee) || isAlignedAllocLike(*callee)))
            {
                return ReturnAllocKind::MallocLike;
            }
            if (isMmapLikeName(name))
            {
                return ReturnAllocKind::MmapLike;
            }
            if (isSbrkLikeName(name))
            {
                return ReturnAllocKind::SbrkLike;
            }
            bool isArray = false;
            OperatorNewKind newKind = OperatorNewKind::Normal;
            if (isOperatorNewName(name, isArray, newKind) && isNewLike(*callee))
            {
                return isArray ? ReturnAllocKind::NewArrayLike : ReturnAllocKind::NewLike;
            }
            return ReturnAllocKind::None;
        }

        CT_NODISCARD llvm::Value* stripPointerCastsAndBitcasts(llvm::Value* value)
        {
            if (!value)
                return nullptr;
            return value->stripPointerCasts();
        }

        CT_NODISCARD ReturnAllocKind classifyReturnAllocKind(const llvm::Function& func)
        {
            ReturnAllocKind kind = ReturnAllocKind::None;
            unsigned retCount = 0;
            for (const llvm::BasicBlock& bb : func)
            {
                const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
                if (!ret)
                    continue;
                ++retCount;
                llvm::Value* retVal = ret->getReturnValue();
                if (!retVal)
                    return ReturnAllocKind::None;
                retVal = stripPointerCastsAndBitcasts(retVal);
                auto* call = llvm::dyn_cast_or_null<llvm::CallBase>(retVal);
                if (!call)
                    return ReturnAllocKind::None;
                ReturnAllocKind retKind = classifyAllocatorCallee(call->getCalledFunction());
                if (retKind == ReturnAllocKind::None)
                    return ReturnAllocKind::None;
                if (kind == ReturnAllocKind::None)
                    kind = retKind;
                else if (kind != retKind)
                    return ReturnAllocKind::None;
            }
            if (retCount == 0)
                return ReturnAllocKind::None;
            return kind;
        }

        CT_NODISCARD bool isPosixMemalignLike(const llvm::Function& fn)
        {
            if (!fn.isDeclaration())
            {
                return false;
            }
            const auto* fnTy = fn.getFunctionType();
            if (!fnTy || fnTy->isVarArg() || fnTy->getNumParams() != 3)
            {
                return false;
            }
            if (!fnTy->getReturnType()->isIntegerTy())
            {
                return false;
            }
            return fnTy->getParamType(0)->isPointerTy() && fnTy->getParamType(1)->isIntegerTy() &&
                   fnTy->getParamType(2)->isIntegerTy();
        }

        CT_NODISCARD bool isAlignedAllocLike(const llvm::Function& fn)
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

        CT_NODISCARD bool isMmapLike(const llvm::Function& fn)
        {
            if (!fn.isDeclaration())
            {
                return false;
            }
            const auto* fnTy = fn.getFunctionType();
            if (!fnTy || fnTy->isVarArg() || fnTy->getNumParams() != 6)
            {
                return false;
            }
            if (!fnTy->getReturnType()->isPointerTy())
            {
                return false;
            }
            return fnTy->getParamType(0)->isPointerTy();
        }

        CT_NODISCARD bool isMunmapLike(const llvm::Function& fn)
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
            if (!fnTy->getReturnType()->isIntegerTy())
            {
                return false;
            }
            return fnTy->getParamType(0)->isPointerTy();
        }

        CT_NODISCARD bool isFreeLikeName(llvm::StringRef name)
        {
            return name == "free" || name == "__ct_free" || name == "__ct_autofree" ||
                   name == "__ct_delete" || name == "__ct_delete_array" ||
                   name == "__ct_delete_nothrow" || name == "__ct_delete_array_nothrow" ||
                   name == "__ct_autofree_delete" || name == "__ct_autofree_delete_array" ||
                   name == "__ct_munmap" || name == "__ct_autofree_munmap";
        }

        CT_NODISCARD bool isLoadFromAlloca(llvm::Value* value, llvm::AllocaInst* alloca)
        {
            auto* load = llvm::dyn_cast<llvm::LoadInst>(value);
            if (!load)
            {
                return false;
            }
            llvm::Value* src = load->getPointerOperand()->stripPointerCasts();
            return src == alloca;
        }

        struct EscapeAnalysisContext
        {
            const llvm::DataLayout& layout;
            llvm::DenseMap<const llvm::Value*, EscapeState> valueCache;
            llvm::DenseMap<const llvm::AllocaInst*, EscapeState> allocaCache;
            llvm::SmallPtrSet<const llvm::Value*, 16> inProgress;
            explicit EscapeAnalysisContext(const llvm::DataLayout& dl)
                : layout(dl), valueCache(), allocaCache(), inProgress()
            {
            }
        };

        CT_NODISCARD EscapeState classifyScalarEscape(llvm::Value* value,
                                                      EscapeAnalysisContext& ctx);
        CT_NODISCARD EscapeState classifyPointerEscape(llvm::Value* value,
                                                       EscapeAnalysisContext& ctx);
        CT_NODISCARD EscapeState classifyAllocaEscape(llvm::AllocaInst* alloca,
                                                      EscapeAnalysisContext& ctx);
        CT_NODISCARD EscapeState promoteState(EscapeState current, EscapeState next,
                                              const char* reason, llvm::Value* value,
                                              llvm::Value* user = nullptr);
        CT_NODISCARD bool isAllocaDead(llvm::AllocaInst* alloca);

        CT_NODISCARD EscapeState classifyAllocaEscape(llvm::AllocaInst* alloca,
                                                      EscapeAnalysisContext& ctx)
        {
            if (auto it = ctx.allocaCache.find(alloca); it != ctx.allocaCache.end())
            {
                return it->second;
            }

            EscapeState state = EscapeState::ReachableLocal;
            llvm::SmallVector<llvm::Value*, 8> worklist;
            llvm::SmallPtrSet<llvm::Value*, 8> visited;
            worklist.push_back(alloca);
            visited.insert(alloca);

            auto finish = [&](EscapeState finalState)
            {
                ctx.allocaCache[alloca] = finalState;
                return finalState;
            };

            while (!worklist.empty())
            {
                llvm::Value* current = worklist.pop_back_val();
                for (llvm::Use& use : current->uses())
                {
                    auto* user = use.getUser();
                    if (llvm::isa<llvm::DbgInfoIntrinsic>(user) ||
                        llvm::isa<llvm::LifetimeIntrinsic>(user))
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
                    if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user))
                    {
                        if (visited.insert(gep).second)
                        {
                            worklist.push_back(gep);
                        }
                        continue;
                    }
                    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (store->getPointerOperand() == current)
                        {
                            continue;
                        }
                        state = promoteState(state, EscapeState::EscapedStore, "escape: store",
                                             alloca, store);
                        return finish(state);
                    }
                    if (auto* load = llvm::dyn_cast<llvm::LoadInst>(user))
                    {
                        for (llvm::Use& loadUse : load->uses())
                        {
                            auto* loadUser = loadUse.getUser();
                            if (llvm::isa<llvm::ReturnInst>(loadUser))
                            {
                                state = promoteState(state, EscapeState::EscapedReturn,
                                                     "escape: return", alloca, loadUser);
                                return finish(state);
                            }
                            if (auto* call = llvm::dyn_cast<llvm::CallBase>(loadUser))
                            {
                                llvm::Function* callee = call->getCalledFunction();
                                if (callee && isFreeLikeName(callee->getName()))
                                {
                                    state = promoteState(state, EscapeState::EscapedCall,
                                                         "escape: free-like", alloca, loadUser);
                                    return finish(state);
                                }
                                if (call->getFunctionType()->isVarArg())
                                {
                                    state = promoteState(state, EscapeState::EscapedCall,
                                                         "escape: call", alloca, loadUser);
                                    return finish(state);
                                }
                                auto captureKind = llvm::DetermineUseCaptureKind(
                                    loadUse,
                                    [&](llvm::Value*, const llvm::DataLayout&) { return false; });
                                if (captureKind != llvm::UseCaptureKind::NO_CAPTURE)
                                {
                                    state = promoteState(state, EscapeState::EscapedCall,
                                                         "escape: call", alloca, loadUser);
                                    return finish(state);
                                }
                                continue;
                            }
                            if (auto* store2 = llvm::dyn_cast<llvm::StoreInst>(loadUser))
                            {
                                llvm::Value* dest =
                                    store2->getPointerOperand()->stripPointerCasts();
                                if (!llvm::isa<llvm::AllocaInst>(dest))
                                {
                                    state = promoteState(state, EscapeState::EscapedStore,
                                                         "escape: store", alloca, loadUser);
                                    return finish(state);
                                }
                            }
                            EscapeState inner = classifyPointerEscape(loadUser, ctx);
                            if (inner != EscapeState::ReachableLocal)
                            {
                                state = promoteState(state, inner, "escape: through load", alloca,
                                                     loadUser);
                                return finish(state);
                            }
                        }
                        continue;
                    }
                    if (llvm::isa<llvm::CallBase>(user) || llvm::isa<llvm::ReturnInst>(user))
                    {
                        state = promoteState(state, EscapeState::EscapedCall, "escape: call",
                                             alloca, user);
                        return finish(state);
                    }
                    state = promoteState(state, EscapeState::EscapedStore, "escape: unknown",
                                         alloca, user);
                    return finish(state);
                }
            }

            return finish(state);
        }

        CT_NODISCARD bool debugAutofreeEnabled(void);
        void logAutofreeDebug(const char* reason, llvm::Value* value, llvm::Value* user);

        void logAutofreeState(const char* action, EscapeState state, llvm::Value* value,
                              llvm::Value* user = nullptr)
        {
            if (!debugAutofreeEnabled())
            {
                return;
            }
            llvm::errs() << "ct-autofree: " << action << " state=" << escapeStateName(state);
            if (auto* inst = llvm::dyn_cast_or_null<llvm::Instruction>(value))
            {
                llvm::errs() << " in " << inst->getFunction()->getName() << " value=" << *inst;
            }
            if (auto* uinst = llvm::dyn_cast_or_null<llvm::Instruction>(user))
            {
                llvm::errs() << " user=" << *uinst;
            }
            llvm::errs() << "\n";
        }

        CT_NODISCARD EscapeState promoteState(EscapeState current, EscapeState next,
                                              const char* reason, llvm::Value* value,
                                              llvm::Value* user)
        {
            if (escapeRank(next) > escapeRank(current))
            {
                logAutofreeState(reason, next, value, user);
                return next;
            }
            return current;
        }

        CT_NODISCARD EscapeState classifyScalarEscape(llvm::Value* value,
                                                      EscapeAnalysisContext& ctx)
        {
            if (auto it = ctx.valueCache.find(value); it != ctx.valueCache.end())
            {
                return it->second;
            }
            if (!ctx.inProgress.insert(value).second)
            {
                return EscapeState::EscapedCall;
            }

            EscapeState state = EscapeState::ReachableLocal;
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
                    if (auto* cast = llvm::dyn_cast<llvm::CastInst>(user))
                    {
                        if (cast->getType()->isPointerTy())
                        {
                            EscapeState ptrState = classifyPointerEscape(cast, ctx);
                            if (ptrState != EscapeState::ReachableLocal)
                            {
                                state =
                                    promoteState(state, ptrState, "escape: inttoptr", value, user);
                                ctx.valueCache[value] = state;
                                ctx.inProgress.erase(value);
                                return state;
                            }
                            continue;
                        }
                        if (visited.insert(cast).second)
                            worklist.push_back(cast);
                        continue;
                    }
                    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(user))
                    {
                        if (visited.insert(phi).second)
                            worklist.push_back(phi);
                        continue;
                    }
                    if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(user))
                    {
                        if (visited.insert(sel).second)
                            worklist.push_back(sel);
                        continue;
                    }
                    if (auto* bin = llvm::dyn_cast<llvm::BinaryOperator>(user))
                    {
                        if (visited.insert(bin).second)
                            worklist.push_back(bin);
                        continue;
                    }
                    if (auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(user))
                    {
                        (void)cmp;
                        continue;
                    }
                    if (auto* br = llvm::dyn_cast<llvm::BranchInst>(user))
                    {
                        if (br->isConditional())
                        {
                            continue;
                        }
                    }
                    if (llvm::isa<llvm::SwitchInst>(user))
                    {
                        continue;
                    }
                    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (store->getValueOperand() == current)
                        {
                            llvm::Value* dest = store->getPointerOperand()->stripPointerCasts();
                            if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(dest))
                            {
                                if (isAllocaDead(alloca))
                                {
                                    continue;
                                }
                            }
                            if (auto* obj = llvm::getUnderlyingObject(dest))
                            {
                                if (llvm::isa<llvm::GlobalValue>(obj))
                                {
                                    state = promoteState(state, EscapeState::ReachableGlobal,
                                                         "escape: store global", value, user);
                                    ctx.valueCache[value] = state;
                                    ctx.inProgress.erase(value);
                                    return state;
                                }
                            }
                        }
                        state = promoteState(state, EscapeState::EscapedStore, "escape: store",
                                             value, user);
                        ctx.valueCache[value] = state;
                        ctx.inProgress.erase(value);
                        return state;
                    }
                    if (llvm::isa<llvm::CallBase>(user))
                    {
                        state = promoteState(state, EscapeState::EscapedCall, "escape: call", value,
                                             user);
                        ctx.valueCache[value] = state;
                        ctx.inProgress.erase(value);
                        return state;
                    }
                    if (llvm::isa<llvm::ReturnInst>(user))
                    {
                        state = promoteState(state, EscapeState::EscapedReturn, "escape: return",
                                             value, user);
                        ctx.valueCache[value] = state;
                        ctx.inProgress.erase(value);
                        return state;
                    }
                    state = promoteState(state, EscapeState::EscapedStore, "escape: unknown", value,
                                         user);
                    ctx.valueCache[value] = state;
                    ctx.inProgress.erase(value);
                    return state;
                }
            }

            ctx.valueCache[value] = state;
            ctx.inProgress.erase(value);
            return state;
        }

        CT_NODISCARD EscapeState classifyPointerEscape(llvm::Value* value,
                                                       EscapeAnalysisContext& ctx)
        {
            if (auto it = ctx.valueCache.find(value); it != ctx.valueCache.end())
            {
                return it->second;
            }
            if (!value->getType()->isPointerTy())
            {
                return classifyScalarEscape(value, ctx);
            }
            if (!ctx.inProgress.insert(value).second)
            {
                return EscapeState::EscapedCall;
            }

            EscapeState state = EscapeState::ReachableLocal;
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
                        continue;
                    if (auto* cast = llvm::dyn_cast<llvm::BitCastInst>(user))
                    {
                        if (visited.insert(cast).second)
                            worklist.push_back(cast);
                        continue;
                    }
                    if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user))
                    {
                        if (visited.insert(gep).second)
                            worklist.push_back(gep);
                        continue;
                    }
                    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(user))
                    {
                        if (visited.insert(phi).second)
                            worklist.push_back(phi);
                        continue;
                    }
                    if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(user))
                    {
                        if (visited.insert(sel).second)
                            worklist.push_back(sel);
                        continue;
                    }
                    if (auto* i2p = llvm::dyn_cast<llvm::IntToPtrInst>(user))
                    {
                        if (visited.insert(i2p).second)
                            worklist.push_back(i2p);
                        continue;
                    }
                    if (auto* cast = llvm::dyn_cast<llvm::AddrSpaceCastInst>(user))
                    {
                        if (visited.insert(cast).second)
                            worklist.push_back(cast);
                        continue;
                    }
                    if (auto* ce = llvm::dyn_cast<llvm::ConstantExpr>(user))
                    {
                        if (ce->getOpcode() == llvm::Instruction::PtrToInt)
                        {
                            EscapeState scalarState = classifyScalarEscape(ce, ctx);
                            if (scalarState != EscapeState::ReachableLocal)
                            {
                                logAutofreeDebug("escape: ptrtoint", value, user);
                                state = promoteState(state, scalarState, "escape: ptrtoint", value,
                                                     user);
                                ctx.valueCache[value] = state;
                                ctx.inProgress.erase(value);
                                return state;
                            }
                            continue;
                        }
                        if (ce->isCast() || ce->getOpcode() == llvm::Instruction::GetElementPtr)
                        {
                            if (visited.insert(ce).second)
                                worklist.push_back(ce);
                            continue;
                        }
                    }
                    if (auto* p2i = llvm::dyn_cast<llvm::PtrToIntInst>(user))
                    {
                        EscapeState scalarState = classifyScalarEscape(p2i, ctx);
                        if (scalarState != EscapeState::ReachableLocal)
                        {
                            logAutofreeDebug("escape: ptrtoint", value, user);
                            state =
                                promoteState(state, scalarState, "escape: ptrtoint", value, user);
                            ctx.valueCache[value] = state;
                            ctx.inProgress.erase(value);
                            return state;
                        }
                        continue;
                    }
                    if (llvm::isa<llvm::ICmpInst>(user))
                    {
                        continue;
                    }
                    if (auto* br = llvm::dyn_cast<llvm::BranchInst>(user))
                    {
                        if (br->isConditional())
                        {
                            // Using the value only as a branch condition is non-escaping.
                            continue;
                        }
                    }
                    if (llvm::isa<llvm::SwitchInst>(user))
                    {
                        continue;
                    }
                    if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(user))
                    {
                        logAutofreeDebug("escape: return", value, user);
                        state = promoteState(state, EscapeState::EscapedReturn, "escape: return",
                                             value, user);
                        ctx.valueCache[value] = state;
                        ctx.inProgress.erase(value);
                        return state;
                    }
                    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (store->getValueOperand() == current)
                        {
                            llvm::Value* dest = store->getPointerOperand()->stripPointerCasts();
                            if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(dest))
                            {
                                EscapeState allocaState = classifyAllocaEscape(alloca, ctx);
                                if (allocaState != EscapeState::ReachableLocal)
                                {
                                    logAutofreeDebug("escape: store to escaping alloca", value,
                                                     store);
                                    state = promoteState(state, allocaState, "escape: alloca",
                                                         value, store);
                                    ctx.valueCache[value] = state;
                                    ctx.inProgress.erase(value);
                                    return state;
                                }
                                continue;
                            }
                            logAutofreeDebug("escape: store to non-alloca", value, store);
                            if (auto* obj = llvm::getUnderlyingObject(dest))
                            {
                                if (llvm::isa<llvm::GlobalValue>(obj))
                                {
                                    state = promoteState(state, EscapeState::ReachableGlobal,
                                                         "escape: store global", value, store);
                                    ctx.valueCache[value] = state;
                                    ctx.inProgress.erase(value);
                                    return state;
                                }
                            }
                            state = promoteState(state, EscapeState::EscapedStore, "escape: store",
                                                 value, store);
                            ctx.valueCache[value] = state;
                            ctx.inProgress.erase(value);
                            return state;
                        }
                        continue;
                    }
                    if (auto* call = llvm::dyn_cast<llvm::CallBase>(user))
                    {
                        llvm::Function* callee = call->getCalledFunction();
                        if (callee && isFreeLikeName(callee->getName()))
                        {
                            state = promoteState(state, EscapeState::EscapedCall,
                                                 "escape: free-like", value, call);
                            ctx.valueCache[value] = state;
                            ctx.inProgress.erase(value);
                            return state;
                        }

                        if (call->getFunctionType()->isVarArg())
                        {
                            logAutofreeDebug("escape: varargs call", value, call);
                            state = promoteState(state, EscapeState::EscapedCall, "escape: call",
                                                 value, call);
                            ctx.valueCache[value] = state;
                            ctx.inProgress.erase(value);
                            return state;
                        }

                        auto captureKind = llvm::DetermineUseCaptureKind(
                            use, [&](llvm::Value*, const llvm::DataLayout&) { return false; });
                        if (captureKind == llvm::UseCaptureKind::NO_CAPTURE)
                        {
                            continue;
                        }
                        if (captureKind == llvm::UseCaptureKind::PASSTHROUGH)
                        {
                            if (call->getType()->isPointerTy() && visited.insert(call).second)
                            {
                                worklist.push_back(call);
                            }
                            continue;
                        }
                        logAutofreeDebug("escape: call", value, call);
                        state = promoteState(state, EscapeState::EscapedCall, "escape: call", value,
                                             call);
                        ctx.valueCache[value] = state;
                        ctx.inProgress.erase(value);
                        return state;
                    }
                    logAutofreeDebug("escape: unknown", value, user);
                    state = promoteState(state, EscapeState::EscapedCall, "escape: unknown", value,
                                         user);
                    ctx.valueCache[value] = state;
                    ctx.inProgress.erase(value);
                    return state;
                }
            }
            ctx.valueCache[value] = state;
            ctx.inProgress.erase(value);
            return state;
        }

        CT_NODISCARD bool isSbrkLike(const llvm::Function& fn)
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

        CT_NODISCARD bool isBrkLike(const llvm::Function& fn)
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
            return fnTy->getParamType(0)->isPointerTy();
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

        CT_NODISCARD bool debugAutofreeEnabled(void)
        {
            static int initialized = 0;
            static bool enabled = false;
            if (!initialized)
            {
                enabled = std::getenv("CT_DEBUG_AUTOFREE") != nullptr;
                initialized = 1;
            }
            return enabled;
        }

        void logAutofreeDebug(const char* reason, llvm::Value* value, llvm::Value* user = nullptr)
        {
            if (!debugAutofreeEnabled())
            {
                return;
            }
            llvm::errs() << "ct-autofree: " << reason;
            if (auto* inst = llvm::dyn_cast_or_null<llvm::Instruction>(value))
            {
                llvm::errs() << " in " << inst->getFunction()->getName() << " value=" << *inst;
            }
            if (auto* uinst = llvm::dyn_cast_or_null<llvm::Instruction>(user))
            {
                llvm::errs() << " user=" << *uinst;
            }
            llvm::errs() << "\n";
        }

        void logAutofreeDecision(const char* reason, llvm::Value* value, ReturnAllocKind kind)
        {
            if (!debugAutofreeEnabled())
            {
                return;
            }
            llvm::errs() << "ct-autofree: " << reason << " kind=" << static_cast<int>(kind);
            if (auto* inst = llvm::dyn_cast_or_null<llvm::Instruction>(value))
            {
                llvm::errs() << " in " << inst->getFunction()->getName() << " value=" << *inst;
            }
            llvm::errs() << "\n";
        }

        CT_NODISCARD bool isOnlyUsedByDebug(const llvm::Instruction* inst)
        {
            if (!inst)
                return false;
            for (const llvm::Use& u : inst->uses())
            {
                if (!llvm::isa<llvm::DbgInfoIntrinsic>(u.getUser()))
                {
                    return false;
                }
            }
            return true;
        }

        CT_NODISCARD bool isAllocaDead(llvm::AllocaInst* alloca)
        {
            llvm::SmallVector<llvm::Value*, 8> worklist;
            llvm::SmallPtrSet<llvm::Value*, 8> visited;

            worklist.push_back(alloca);
            visited.insert(alloca);

            while (!worklist.empty())
            {
                llvm::Value* current = worklist.pop_back_val();
                for (llvm::Use& use : current->uses())
                {
                    auto* user = use.getUser();
                    if (llvm::isa<llvm::DbgInfoIntrinsic>(user) ||
                        llvm::isa<llvm::LifetimeIntrinsic>(user))
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
                    if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user))
                    {
                        if (visited.insert(gep).second)
                        {
                            worklist.push_back(gep);
                        }
                        continue;
                    }
                    if (auto* ce = llvm::dyn_cast<llvm::ConstantExpr>(user))
                    {
                        if (ce->isCast() || ce->getOpcode() == llvm::Instruction::GetElementPtr)
                        {
                            if (visited.insert(ce).second)
                            {
                                worklist.push_back(ce);
                            }
                            continue;
                        }
                    }
                    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        // OK only if the alloca (or its derived address) is the destination.
                        if (store->getPointerOperand() == current)
                        {
                            continue;
                        }
                        logAutofreeDebug("escape: store non-local", alloca, store);
                        return false;
                    }
                    if (auto* load = llvm::dyn_cast<llvm::LoadInst>(user))
                    {
                        // Clang -O0/-g may introduce debug-only loads feeding dbg.value; those do
                        // not make the alloca "live" for escape purposes.
                        if (isOnlyUsedByDebug(load))
                        {
                            continue;
                        }
                        logAutofreeDebug("escape: load", alloca, user);
                        return false;
                    }
                    if (llvm::isa<llvm::CallBase>(user) || llvm::isa<llvm::InvokeInst>(user) ||
                        llvm::isa<llvm::ReturnInst>(user))
                    {
                        logAutofreeDebug("escape: call/return", alloca, user);
                        return false;
                    }
                    return false;
                }
            }

            return true;
        }

        CT_NODISCARD bool isEffectivelyUnused(llvm::Value* value, const llvm::DataLayout& layout)
        {
            (void)layout;

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
                    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (store->getValueOperand() == current)
                        {
                            llvm::Value* dest = store->getPointerOperand()->stripPointerCasts();
                            if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(dest))
                            {
                                if (isAllocaDead(alloca))
                                {
                                    continue;
                                }
                            }
                            logAutofreeDebug("escape: stored to non-dead alloca", value, store);
                        }
                        return false;
                    }
                    if (auto* call = llvm::dyn_cast<llvm::CallBase>(user))
                    {
                        // Ignore auto-free calls injected by this pass so unreachable classification
                        // remains based on user-visible uses.
                        if (llvm::Function* callee = call->getCalledFunction())
                        {
                            if (callee->getName().starts_with("__ct_autofree"))
                            {
                                continue;
                            }
                        }
                        return false;
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
        EscapeAnalysisContext escapeCtx(layout);
        llvm::Type* voidPtrTy = llvm::PointerType::get(llvm::Type::getInt8Ty(context), 0);
        llvm::Type* sizeTy = layout.getIntPtrType(context);
        llvm::Type* intTy = llvm::Type::getInt32Ty(context);

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
        llvm::FunctionCallee ctNewNothrow =
            module.getOrInsertFunction("__ct_new_nothrow", mallocTy);
        llvm::FunctionCallee ctNewNothrowUnreachable =
            module.getOrInsertFunction("__ct_new_nothrow_unreachable", mallocTy);
        llvm::FunctionCallee ctNewArrayNothrow =
            module.getOrInsertFunction("__ct_new_array_nothrow", mallocTy);
        llvm::FunctionCallee ctNewArrayNothrowUnreachable =
            module.getOrInsertFunction("__ct_new_array_nothrow_unreachable", mallocTy);
        llvm::Type* voidPtrPtrTy = llvm::PointerType::get(voidPtrTy, 0);
        llvm::FunctionCallee ctFree = module.getOrInsertFunction("__ct_free", freeTy);
        llvm::FunctionCallee ctDelete = module.getOrInsertFunction("__ct_delete", freeTy);
        llvm::FunctionCallee ctDeleteArray =
            module.getOrInsertFunction("__ct_delete_array", freeTy);
        llvm::FunctionCallee ctDeleteNothrow =
            module.getOrInsertFunction("__ct_delete_nothrow", freeTy);
        llvm::FunctionCallee ctDeleteArrayNothrow =
            module.getOrInsertFunction("__ct_delete_array_nothrow", freeTy);
        llvm::FunctionCallee ctDeleteDestroying =
            module.getOrInsertFunction("__ct_delete_destroying", freeTy);
        llvm::FunctionCallee ctDeleteArrayDestroying =
            module.getOrInsertFunction("__ct_delete_array_destroying", freeTy);
        llvm::FunctionCallee ctAutoFree = module.getOrInsertFunction("__ct_autofree", freeTy);
        llvm::FunctionCallee ctAutoFreeDelete =
            module.getOrInsertFunction("__ct_autofree_delete", freeTy);
        llvm::FunctionCallee ctAutoFreeDeleteArray =
            module.getOrInsertFunction("__ct_autofree_delete_array", freeTy);
        auto* posixMemalignTy =
            llvm::FunctionType::get(intTy, {voidPtrPtrTy, sizeTy, sizeTy, voidPtrTy}, false);
        llvm::FunctionCallee ctPosixMemalign =
            module.getOrInsertFunction("__ct_posix_memalign", posixMemalignTy);
        auto* alignedAllocTy =
            llvm::FunctionType::get(voidPtrTy, {sizeTy, sizeTy, voidPtrTy}, false);
        llvm::FunctionCallee ctAlignedAlloc =
            module.getOrInsertFunction("__ct_aligned_alloc", alignedAllocTy);
        llvm::FunctionCallee ctMmap = module.getOrInsertFunction(
            "__ct_mmap",
            llvm::FunctionType::get(
                voidPtrTy, {voidPtrTy, sizeTy, intTy, intTy, intTy, sizeTy, voidPtrTy}, false));
        auto* munmapTy = llvm::FunctionType::get(intTy, {voidPtrTy, sizeTy, voidPtrTy}, false);
        llvm::FunctionCallee ctMunmap = module.getOrInsertFunction("__ct_munmap", munmapTy);
        auto* sbrkTy = llvm::FunctionType::get(voidPtrTy, {sizeTy, voidPtrTy}, false);
        llvm::FunctionCallee ctSbrk = module.getOrInsertFunction("__ct_sbrk", sbrkTy);
        auto* brkTy = llvm::FunctionType::get(voidPtrTy, {voidPtrTy, voidPtrTy}, false);
        llvm::FunctionCallee ctBrk = module.getOrInsertFunction("__ct_brk", brkTy);
        llvm::FunctionCallee ctAutoFreeMunmap =
            module.getOrInsertFunction("__ct_autofree_munmap", freeTy);

        llvm::DenseMap<const llvm::DILocation*, llvm::Constant*> siteCache;
        llvm::Constant* unknownSite = nullptr;
        llvm::SmallVector<llvm::CallBase*, 16> mallocCalls;
        llvm::SmallVector<llvm::CallBase*, 16> callocCalls;
        llvm::SmallVector<llvm::CallBase*, 16> reallocCalls;
        llvm::SmallVector<llvm::CallBase*, 16> posixMemalignCalls;
        llvm::SmallVector<llvm::WeakTrackingVH, 16> alignedAllocCalls;
        llvm::SmallVector<llvm::WeakTrackingVH, 16> mmapCalls;
        llvm::SmallVector<llvm::CallBase*, 16> munmapCalls;
        llvm::SmallVector<llvm::WeakTrackingVH, 16> sbrkCalls;
        llvm::SmallVector<llvm::CallBase*, 16> brkCalls;
        llvm::SmallVector<llvm::CallBase*, 16> freeCalls;
        llvm::SmallVector<llvm::CallBase*, 16> newCalls;
        llvm::SmallVector<llvm::CallBase*, 16> newArrayCalls;
        llvm::SmallVector<llvm::CallBase*, 16> newNothrowCalls;
        llvm::SmallVector<llvm::CallBase*, 16> newArrayNothrowCalls;
        llvm::SmallVector<llvm::CallBase*, 16> deleteCalls;
        llvm::SmallVector<llvm::CallBase*, 16> deleteArrayCalls;
        llvm::SmallVector<llvm::CallBase*, 16> deleteNothrowCalls;
        llvm::SmallVector<llvm::CallBase*, 16> deleteArrayNothrowCalls;
        llvm::SmallVector<llvm::CallBase*, 16> deleteDestroyingCalls;
        llvm::SmallVector<llvm::CallBase*, 16> deleteArrayDestroyingCalls;
        struct AllocSite
        {
            llvm::Value* value = nullptr;
            llvm::AllocaInst* outAlloca = nullptr;
            ReturnAllocKind kind = ReturnAllocKind::None;
        };
        llvm::SmallVector<AllocSite, 32> allocSites;
        llvm::SmallVector<llvm::CallBase*, 16> unusedResultCalls;
        llvm::DenseMap<const llvm::Function*, ReturnAllocKind> returnsOwned;
        llvm::SmallPtrSet<const llvm::Value*, 32> instantAutoFreeValues;

        for (llvm::Function& func : module)
        {
            if (!shouldInstrument(func))
            {
                continue;
            }
            ReturnAllocKind kind = classifyReturnAllocKind(func);
            if (kind != ReturnAllocKind::None)
            {
                returnsOwned.try_emplace(&func, kind);
            }
        }

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
                            allocSites.push_back({call, nullptr, ReturnAllocKind::MallocLike});
                            if (isEffectivelyUnused(call, layout))
                                instantAutoFreeValues.insert(call);
                        }
                        continue;
                    }
                    if (name == "calloc")
                    {
                        if (isCallocLike(*callee))
                        {
                            callocCalls.push_back(call);
                            allocSites.push_back({call, nullptr, ReturnAllocKind::MallocLike});
                            if (isEffectivelyUnused(call, layout))
                                instantAutoFreeValues.insert(call);
                        }
                        continue;
                    }
                    if (name == "posix_memalign")
                    {
                        if (isPosixMemalignLike(*callee))
                        {
                            posixMemalignCalls.push_back(call);
                            if (auto* outAlloca = llvm::dyn_cast<llvm::AllocaInst>(
                                    call->getArgOperand(0)->stripPointerCasts()))
                            {
                                allocSites.push_back(
                                    {nullptr, outAlloca, ReturnAllocKind::MallocLike});
                            }
                        }
                        continue;
                    }
                    if (name == "realloc")
                    {
                        if (isReallocLike(*callee))
                        {
                            reallocCalls.push_back(call);
                            allocSites.push_back({call, nullptr, ReturnAllocKind::MallocLike});
                        }
                        continue;
                    }
                    if (name == "aligned_alloc")
                    {
                        if (isAlignedAllocLike(*callee))
                        {
                            alignedAllocCalls.push_back(call);
                            allocSites.push_back({call, nullptr, ReturnAllocKind::MallocLike});
                            if (isEffectivelyUnused(call, layout))
                                instantAutoFreeValues.insert(call);
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
                    if (isMmapLikeName(name))
                    {
                        if (isMmapLike(*callee))
                        {
                            mmapCalls.push_back(call);
                            allocSites.push_back({call, nullptr, ReturnAllocKind::MmapLike});
                            if (isEffectivelyUnused(call, layout))
                                instantAutoFreeValues.insert(call);
                        }
                        continue;
                    }
                    if (isMunmapLikeName(name))
                    {
                        if (isMunmapLike(*callee))
                        {
                            munmapCalls.push_back(call);
                        }
                        continue;
                    }
                    if (isSbrkLikeName(name))
                    {
                        if (isSbrkLike(*callee))
                        {
                            sbrkCalls.push_back(call);
                            allocSites.push_back({call, nullptr, ReturnAllocKind::SbrkLike});
                            if (isEffectivelyUnused(call, layout))
                                instantAutoFreeValues.insert(call);
                        }
                        continue;
                    }
                    if (isBrkLikeName(name))
                    {
                        if (isBrkLike(*callee))
                        {
                            brkCalls.push_back(call);
                        }
                        continue;
                    }

                    bool isArray = false;
                    OperatorNewKind newKind = OperatorNewKind::Normal;
                    if (isOperatorNewName(name, isArray, newKind) && isNewLike(*callee))
                    {
                        if (newKind == OperatorNewKind::Nothrow)
                        {
                            if (isArray)
                                newArrayNothrowCalls.push_back(call);
                            else
                                newNothrowCalls.push_back(call);
                        }
                        else
                        {
                            if (isArray)
                                newArrayCalls.push_back(call);
                            else
                                newCalls.push_back(call);
                        }
                        allocSites.push_back(
                            {call, nullptr,
                             isArray ? ReturnAllocKind::NewArrayLike : ReturnAllocKind::NewLike});
                        if (isEffectivelyUnused(call, layout))
                            instantAutoFreeValues.insert(call);
                        continue;
                    }
                    OperatorDeleteKind delKind = OperatorDeleteKind::Normal;
                    if (isOperatorDeleteName(name, isArray, delKind) && isDeleteLike(*callee))
                    {
                        if (delKind == OperatorDeleteKind::Destroying)
                        {
                            if (isArray)
                                deleteArrayDestroyingCalls.push_back(call);
                            else
                                deleteDestroyingCalls.push_back(call);
                        }
                        else if (delKind == OperatorDeleteKind::Nothrow)
                        {
                            if (isArray)
                                deleteArrayNothrowCalls.push_back(call);
                            else
                                deleteNothrowCalls.push_back(call);
                        }
                        else
                        {
                            if (isArray)
                                deleteArrayCalls.push_back(call);
                            else
                                deleteCalls.push_back(call);
                        }
                    }

                    if (isEffectivelyUnused(call, layout))
                    {
                        if (auto it = returnsOwned.find(callee); it != returnsOwned.end())
                        {
                            (void)it;
                            unusedResultCalls.push_back(call);
                        }
                    }
                }
            }
        }

        for (llvm::CallBase* call : unusedResultCalls)
        {
            if (!isEffectivelyUnused(call, layout))
                continue;
            llvm::Function* callee = getCalledFunction(*call);
            ReturnAllocKind kind = ReturnAllocKind::None;
            if (auto it = returnsOwned.find(callee); it != returnsOwned.end())
            {
                kind = it->second;
            }
            if (kind == ReturnAllocKind::None)
                continue;
            auto* callInst = llvm::dyn_cast<llvm::CallInst>(call);
            if (!callInst)
                continue;
            llvm::Instruction* insertPt = callInst->getNextNode();
            if (!insertPt)
            {
                insertPt = callInst->getParent()->getTerminator();
            }
            llvm::IRBuilder<> builder(insertPt);
            llvm::Value* ptr = callInst;
            if (ptr->getType() != voidPtrTy)
            {
                ptr = builder.CreateBitCast(ptr, voidPtrTy);
            }
            if (kind == ReturnAllocKind::NewArrayLike)
            {
                builder.CreateCall(ctAutoFreeDeleteArray, {ptr});
            }
            else if (kind == ReturnAllocKind::NewLike)
            {
                builder.CreateCall(ctAutoFreeDelete, {ptr});
            }
            else if (kind == ReturnAllocKind::MmapLike)
            {
                builder.CreateCall(ctAutoFreeMunmap, {ptr});
            }
            else if (kind == ReturnAllocKind::SbrkLike)
            {
                llvm::FunctionCallee ctAutoFreeSbrk =
                    module.getOrInsertFunction("__ct_autofree_sbrk", freeTy);
                builder.CreateCall(ctAutoFreeSbrk, {ptr});
            }
            else
            {
                builder.CreateCall(ctAutoFree, {ptr});
            }
            logAutofreeState("autofree-immediate", EscapeState::Unreachable, ptr, nullptr);
        }

        for (llvm::Function& func : module)
        {
            if (!shouldInstrument(func))
            {
                continue;
            }
            llvm::SmallVector<AllocSite, 16> localSites;
            for (const auto& site : allocSites)
            {
                if (site.value)
                {
                    if (instantAutoFreeValues.contains(site.value))
                    {
                        continue;
                    }
                    if (auto* inst = llvm::dyn_cast<llvm::Instruction>(site.value))
                    {
                        if (inst->getFunction() == &func)
                        {
                            localSites.push_back(site);
                        }
                    }
                }
                else if (site.outAlloca && site.outAlloca->getFunction() == &func)
                {
                    if (instantAutoFreeValues.contains(site.outAlloca))
                    {
                        continue;
                    }
                    localSites.push_back(site);
                }
            }
            if (localSites.empty())
                continue;

            llvm::SmallVector<llvm::ReturnInst*, 4> returns;
            for (llvm::BasicBlock& bb : func)
            {
                if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator()))
                {
                    returns.push_back(ret);
                }
            }
            if (returns.empty())
                continue;

            for (const auto& site : localSites)
            {
                EscapeState state = EscapeState::ReachableLocal;
                if (site.value)
                {
                    logAutofreeDecision("candidate", site.value, site.kind);
                    state = classifyPointerEscape(site.value, escapeCtx);
                }
                else if (site.outAlloca)
                {
                    logAutofreeDecision("candidate-alloca", site.outAlloca, site.kind);
                    state = classifyAllocaEscape(site.outAlloca, escapeCtx);
                }
                if (state != EscapeState::ReachableLocal)
                {
                    if (site.value)
                    {
                        logAutofreeDecision("escape", site.value, site.kind);
                        logAutofreeState("state", state, site.value, nullptr);
                    }
                    else if (site.outAlloca)
                    {
                        logAutofreeDecision("escape-alloca", site.outAlloca, site.kind);
                        logAutofreeState("state", state, site.outAlloca, nullptr);
                    }
                    continue;
                }

                for (llvm::ReturnInst* ret : returns)
                {
                    llvm::IRBuilder<> builder(ret);
                    llvm::Value* ptr = nullptr;
                    if (site.value)
                    {
                        ptr = site.value;
                    }
                    else if (site.outAlloca)
                    {
                        ptr = builder.CreateLoad(voidPtrTy, site.outAlloca);
                    }
                    if (!ptr)
                        continue;
                    if (ptr->getType() != voidPtrTy)
                    {
                        ptr = builder.CreateBitCast(ptr, voidPtrTy);
                    }
                    if (site.kind == ReturnAllocKind::NewArrayLike)
                    {
                        builder.CreateCall(ctAutoFreeDeleteArray, {ptr});
                    }
                    else if (site.kind == ReturnAllocKind::NewLike)
                    {
                        builder.CreateCall(ctAutoFreeDelete, {ptr});
                    }
                    else if (site.kind == ReturnAllocKind::MmapLike)
                    {
                        builder.CreateCall(ctAutoFreeMunmap, {ptr});
                    }
                    else if (site.kind == ReturnAllocKind::SbrkLike)
                    {
                        llvm::FunctionCallee ctAutoFreeSbrk =
                            module.getOrInsertFunction("__ct_autofree_sbrk", freeTy);
                        builder.CreateCall(ctAutoFreeSbrk, {ptr});
                    }
                    else
                    {
                        builder.CreateCall(ctAutoFree, {ptr});
                    }
                    logAutofreeDecision("inserted", ptr, site.kind);
                    logAutofreeState("autofree-return", EscapeState::ReachableLocal, ptr, nullptr);
                }
            }
        }

        for (llvm::CallBase* call : mallocCalls)
        {
            bool unused = instantAutoFreeValues.contains(call);
            llvm::IRBuilder<> builder(call);
            llvm::Value* sizeArg = call->getArgOperand(0);
            if (sizeArg->getType() != sizeTy)
            {
                sizeArg = builder.CreateZExtOrTrunc(sizeArg, sizeTy);
            }

            llvm::Value* site = getSiteString(module, *call, siteCache, unknownSite);
            llvm::FunctionCallee target = unused ? ctMallocUnreachable : ctMalloc;
            llvm::CallBase* newCall = replaceCall(call, target, {sizeArg, site});
            if (unused && newCall)
            {
                llvm::Instruction* insertPt = newCall->getNextNode();
                if (!insertPt)
                {
                    insertPt = newCall->getParent()->getTerminator();
                }
                if (insertPt)
                {
                    llvm::IRBuilder<> afterBuilder(insertPt);
                    llvm::Value* ptr = newCall;
                    if (ptr->getType() != voidPtrTy)
                    {
                        ptr = afterBuilder.CreateBitCast(ptr, voidPtrTy);
                    }
                    afterBuilder.CreateCall(ctAutoFree, {ptr});
                    logAutofreeState("autofree-immediate", EscapeState::Unreachable, ptr, nullptr);
                }
            }
        }

        for (llvm::CallBase* call : callocCalls)
        {
            bool unused = instantAutoFreeValues.contains(call);
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
            llvm::FunctionCallee target = unused ? ctCallocUnreachable : ctCalloc;
            llvm::CallBase* newCall = replaceCall(call, target, {countArg, sizeArg, site});
            if (unused && newCall)
            {
                llvm::Instruction* insertPt = newCall->getNextNode();
                if (!insertPt)
                {
                    insertPt = newCall->getParent()->getTerminator();
                }
                if (insertPt)
                {
                    llvm::IRBuilder<> afterBuilder(insertPt);
                    llvm::Value* ptr = newCall;
                    if (ptr->getType() != voidPtrTy)
                    {
                        ptr = afterBuilder.CreateBitCast(ptr, voidPtrTy);
                    }
                    afterBuilder.CreateCall(ctAutoFree, {ptr});
                    logAutofreeState("autofree-immediate", EscapeState::Unreachable, ptr, nullptr);
                }
            }
        }

        for (llvm::CallBase* call : posixMemalignCalls)
        {
            llvm::IRBuilder<> builder(call);
            llvm::Value* outArg = call->getArgOperand(0);
            llvm::Value* alignArg = call->getArgOperand(1);
            llvm::Value* sizeArg = call->getArgOperand(2);
            if (outArg->getType() != voidPtrPtrTy)
            {
                outArg = builder.CreateBitCast(outArg, voidPtrPtrTy);
            }
            if (alignArg->getType() != sizeTy)
            {
                alignArg = builder.CreateZExtOrTrunc(alignArg, sizeTy);
            }
            if (sizeArg->getType() != sizeTy)
            {
                sizeArg = builder.CreateZExtOrTrunc(sizeArg, sizeTy);
            }
            llvm::Value* site = getSiteString(module, *call, siteCache, unknownSite);
            llvm::CallBase* newCall =
                replaceCall(call, ctPosixMemalign, {outArg, alignArg, sizeArg, site});

            if (auto* outAlloca = llvm::dyn_cast<llvm::AllocaInst>(outArg->stripPointerCasts()))
            {
                if (isAllocaDead(outAlloca))
                {
                    instantAutoFreeValues.insert(outAlloca);
                    llvm::Instruction* insertPt = newCall ? newCall->getNextNode() : nullptr;
                    if (!insertPt)
                    {
                        insertPt = newCall ? newCall->getParent()->getTerminator() : nullptr;
                    }
                    if (!insertPt)
                    {
                        continue;
                    }
                    llvm::IRBuilder<> afterBuilder(insertPt);
                    llvm::Value* loaded = afterBuilder.CreateLoad(voidPtrTy, outAlloca);
                    afterBuilder.CreateCall(ctAutoFree, {loaded});
                    logAutofreeState("autofree-immediate", EscapeState::Unreachable, loaded,
                                     nullptr);
                }
            }
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

        for (const auto& handle : alignedAllocCalls)
        {
            auto* call = llvm::dyn_cast_or_null<llvm::CallBase>(handle);
            if (!call)
            {
                continue;
            }
            bool unused = instantAutoFreeValues.contains(call);
            llvm::IRBuilder<> builder(call);
            llvm::Value* alignArg = call->getArgOperand(0);
            llvm::Value* sizeArg = call->getArgOperand(1);
            if (alignArg->getType() != sizeTy)
            {
                alignArg = builder.CreateZExtOrTrunc(alignArg, sizeTy);
            }
            if (sizeArg->getType() != sizeTy)
            {
                sizeArg = builder.CreateZExtOrTrunc(sizeArg, sizeTy);
            }
            llvm::Value* site = getSiteString(module, *call, siteCache, unknownSite);
            llvm::CallBase* newCall = replaceCall(call, ctAlignedAlloc, {alignArg, sizeArg, site});
            if (unused && newCall)
            {
                llvm::Instruction* insertPt = newCall->getNextNode();
                if (!insertPt)
                {
                    insertPt = newCall->getParent()->getTerminator();
                }
                if (!insertPt)
                {
                    continue;
                }
                llvm::IRBuilder<> afterBuilder(insertPt);
                llvm::Value* ptr = newCall;
                if (ptr->getType() != voidPtrTy)
                {
                    ptr = afterBuilder.CreateBitCast(ptr, voidPtrTy);
                }
                afterBuilder.CreateCall(ctAutoFree, {ptr});
                logAutofreeState("autofree-immediate", EscapeState::Unreachable, ptr, nullptr);
            }
        }

        for (const auto& handle : mmapCalls)
        {
            auto* call = llvm::dyn_cast_or_null<llvm::CallBase>(handle);
            if (!call)
            {
                continue;
            }
            bool unused = instantAutoFreeValues.contains(call);
            llvm::IRBuilder<> builder(call);
            llvm::Value* addrArg = call->getArgOperand(0);
            llvm::Value* lenArg = call->getArgOperand(1);
            llvm::Value* protArg = call->getArgOperand(2);
            llvm::Value* flagsArg = call->getArgOperand(3);
            llvm::Value* fdArg = call->getArgOperand(4);
            llvm::Value* offArg = call->getArgOperand(5);
            if (addrArg->getType() != voidPtrTy)
            {
                addrArg = builder.CreateBitCast(addrArg, voidPtrTy);
            }
            if (lenArg->getType() != sizeTy)
            {
                lenArg = builder.CreateZExtOrTrunc(lenArg, sizeTy);
            }
            if (protArg->getType() != intTy)
            {
                protArg = builder.CreateZExtOrTrunc(protArg, intTy);
            }
            if (flagsArg->getType() != intTy)
            {
                flagsArg = builder.CreateZExtOrTrunc(flagsArg, intTy);
            }
            if (fdArg->getType() != intTy)
            {
                fdArg = builder.CreateZExtOrTrunc(fdArg, intTy);
            }
            if (offArg->getType() != sizeTy)
            {
                offArg = builder.CreateZExtOrTrunc(offArg, sizeTy);
            }
            llvm::Value* site = getSiteString(module, *call, siteCache, unknownSite);
            llvm::CallBase* newCall = replaceCall(
                call, ctMmap, {addrArg, lenArg, protArg, flagsArg, fdArg, offArg, site});
            if (unused && newCall)
            {
                llvm::Instruction* insertPt = newCall->getNextNode();
                if (!insertPt)
                {
                    insertPt = newCall->getParent()->getTerminator();
                }
                if (!insertPt)
                {
                    continue;
                }
                llvm::IRBuilder<> afterBuilder(insertPt);
                llvm::Value* ptr = newCall;
                if (ptr->getType() != voidPtrTy)
                {
                    ptr = afterBuilder.CreateBitCast(ptr, voidPtrTy);
                }
                afterBuilder.CreateCall(ctAutoFreeMunmap, {ptr});
                logAutofreeState("autofree-immediate", EscapeState::Unreachable, ptr, nullptr);
            }
        }

        for (llvm::CallBase* call : munmapCalls)
        {
            llvm::IRBuilder<> builder(call);
            llvm::Value* addrArg = call->getArgOperand(0);
            llvm::Value* lenArg = call->getArgOperand(1);
            if (addrArg->getType() != voidPtrTy)
            {
                addrArg = builder.CreateBitCast(addrArg, voidPtrTy);
            }
            if (lenArg->getType() != sizeTy)
            {
                lenArg = builder.CreateZExtOrTrunc(lenArg, sizeTy);
            }
            llvm::Value* site = getSiteString(module, *call, siteCache, unknownSite);
            (void)replaceCall(call, ctMunmap, {addrArg, lenArg, site});
        }

        for (const auto& handle : sbrkCalls)
        {
            auto* call = llvm::dyn_cast_or_null<llvm::CallBase>(handle);
            if (!call)
            {
                continue;
            }
            bool unused = instantAutoFreeValues.contains(call);
            llvm::IRBuilder<> builder(call);
            llvm::Value* incrArg = call->getArgOperand(0);
            if (incrArg->getType() != sizeTy)
            {
                incrArg = builder.CreateSExtOrTrunc(incrArg, sizeTy);
            }
            llvm::Value* site = getSiteString(module, *call, siteCache, unknownSite);
            llvm::CallBase* newCall = replaceCall(call, ctSbrk, {incrArg, site});
            if (unused && newCall)
            {
                llvm::Instruction* insertPt = newCall->getNextNode();
                if (!insertPt)
                {
                    insertPt = newCall->getParent()->getTerminator();
                }
                if (!insertPt)
                {
                    continue;
                }
                llvm::IRBuilder<> afterBuilder(insertPt);
                llvm::Value* ptr = newCall;
                if (ptr->getType() != voidPtrTy)
                {
                    ptr = afterBuilder.CreateBitCast(ptr, voidPtrTy);
                }
                llvm::FunctionCallee ctAutoFreeSbrk =
                    module.getOrInsertFunction("__ct_autofree_sbrk", freeTy);
                afterBuilder.CreateCall(ctAutoFreeSbrk, {ptr});
                logAutofreeState("autofree-immediate", EscapeState::Unreachable, ptr, nullptr);
            }
        }

        for (llvm::CallBase* call : brkCalls)
        {
            llvm::IRBuilder<> builder(call);
            llvm::Value* addrArg = call->getArgOperand(0);
            if (addrArg->getType() != voidPtrTy)
            {
                addrArg = builder.CreateBitCast(addrArg, voidPtrTy);
            }
            llvm::Value* site = getSiteString(module, *call, siteCache, unknownSite);
            (void)replaceCall(call, ctBrk, {addrArg, site});
        }

        for (llvm::CallBase* call : newCalls)
        {
            bool unused = instantAutoFreeValues.contains(call);
            llvm::IRBuilder<> builder(call);
            llvm::Value* sizeArg = call->getArgOperand(0);
            if (sizeArg->getType() != sizeTy)
            {
                sizeArg = builder.CreateZExtOrTrunc(sizeArg, sizeTy);
            }
            llvm::Value* site = getSiteString(module, *call, siteCache, unknownSite);
            llvm::FunctionCallee target = unused ? ctNewUnreachable : ctNew;
            llvm::CallBase* newCall = replaceCall(call, target, {sizeArg, site});
            if (unused && newCall)
            {
                llvm::Instruction* insertPt = newCall->getNextNode();
                if (!insertPt)
                {
                    insertPt = newCall->getParent()->getTerminator();
                }
                if (insertPt)
                {
                    llvm::IRBuilder<> afterBuilder(insertPt);
                    llvm::Value* ptr = newCall;
                    if (ptr->getType() != voidPtrTy)
                    {
                        ptr = afterBuilder.CreateBitCast(ptr, voidPtrTy);
                    }
                    afterBuilder.CreateCall(ctAutoFreeDelete, {ptr});
                    logAutofreeState("autofree-immediate", EscapeState::Unreachable, ptr, nullptr);
                }
            }
        }

        for (llvm::CallBase* call : newArrayCalls)
        {
            bool unused = instantAutoFreeValues.contains(call);
            llvm::IRBuilder<> builder(call);
            llvm::Value* sizeArg = call->getArgOperand(0);
            if (sizeArg->getType() != sizeTy)
            {
                sizeArg = builder.CreateZExtOrTrunc(sizeArg, sizeTy);
            }
            llvm::Value* site = getSiteString(module, *call, siteCache, unknownSite);
            llvm::FunctionCallee target = unused ? ctNewArrayUnreachable : ctNewArray;
            llvm::CallBase* newCall = replaceCall(call, target, {sizeArg, site});
            if (unused && newCall)
            {
                llvm::Instruction* insertPt = newCall->getNextNode();
                if (!insertPt)
                {
                    insertPt = newCall->getParent()->getTerminator();
                }
                if (insertPt)
                {
                    llvm::IRBuilder<> afterBuilder(insertPt);
                    llvm::Value* ptr = newCall;
                    if (ptr->getType() != voidPtrTy)
                    {
                        ptr = afterBuilder.CreateBitCast(ptr, voidPtrTy);
                    }
                    afterBuilder.CreateCall(ctAutoFreeDeleteArray, {ptr});
                    logAutofreeState("autofree-immediate", EscapeState::Unreachable, ptr, nullptr);
                }
            }
        }

        for (llvm::CallBase* call : newNothrowCalls)
        {
            bool unused = instantAutoFreeValues.contains(call);
            llvm::IRBuilder<> builder(call);
            llvm::Value* sizeArg = call->getArgOperand(0);
            if (sizeArg->getType() != sizeTy)
            {
                sizeArg = builder.CreateZExtOrTrunc(sizeArg, sizeTy);
            }
            llvm::Value* site = getSiteString(module, *call, siteCache, unknownSite);
            llvm::FunctionCallee target = unused ? ctNewNothrowUnreachable : ctNewNothrow;
            llvm::CallBase* newCall = replaceCall(call, target, {sizeArg, site});
            if (unused && newCall)
            {
                llvm::Instruction* insertPt = newCall->getNextNode();
                if (!insertPt)
                {
                    insertPt = newCall->getParent()->getTerminator();
                }
                if (insertPt)
                {
                    llvm::IRBuilder<> afterBuilder(insertPt);
                    llvm::Value* ptr = newCall;
                    if (ptr->getType() != voidPtrTy)
                    {
                        ptr = afterBuilder.CreateBitCast(ptr, voidPtrTy);
                    }
                    afterBuilder.CreateCall(ctAutoFreeDelete, {ptr});
                    logAutofreeState("autofree-immediate", EscapeState::Unreachable, ptr, nullptr);
                }
            }
        }

        for (llvm::CallBase* call : newArrayNothrowCalls)
        {
            bool unused = instantAutoFreeValues.contains(call);
            llvm::IRBuilder<> builder(call);
            llvm::Value* sizeArg = call->getArgOperand(0);
            if (sizeArg->getType() != sizeTy)
            {
                sizeArg = builder.CreateZExtOrTrunc(sizeArg, sizeTy);
            }
            llvm::Value* site = getSiteString(module, *call, siteCache, unknownSite);
            llvm::FunctionCallee target = unused ? ctNewArrayNothrowUnreachable : ctNewArrayNothrow;
            llvm::CallBase* newCall = replaceCall(call, target, {sizeArg, site});
            if (unused && newCall)
            {
                llvm::Instruction* insertPt = newCall->getNextNode();
                if (!insertPt)
                {
                    insertPt = newCall->getParent()->getTerminator();
                }
                if (insertPt)
                {
                    llvm::IRBuilder<> afterBuilder(insertPt);
                    llvm::Value* ptr = newCall;
                    if (ptr->getType() != voidPtrTy)
                    {
                        ptr = afterBuilder.CreateBitCast(ptr, voidPtrTy);
                    }
                    afterBuilder.CreateCall(ctAutoFreeDeleteArray, {ptr});
                    logAutofreeState("autofree-immediate", EscapeState::Unreachable, ptr, nullptr);
                }
            }
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

        for (llvm::CallBase* call : deleteNothrowCalls)
        {
            llvm::IRBuilder<> builder(call);
            llvm::Value* ptrArg = call->getArgOperand(0);
            if (ptrArg->getType() != voidPtrTy)
            {
                ptrArg = builder.CreateBitCast(ptrArg, voidPtrTy);
            }
            (void)replaceCall(call, ctDeleteNothrow, {ptrArg});
        }

        for (llvm::CallBase* call : deleteArrayNothrowCalls)
        {
            llvm::IRBuilder<> builder(call);
            llvm::Value* ptrArg = call->getArgOperand(0);
            if (ptrArg->getType() != voidPtrTy)
            {
                ptrArg = builder.CreateBitCast(ptrArg, voidPtrTy);
            }
            (void)replaceCall(call, ctDeleteArrayNothrow, {ptrArg});
        }

        for (llvm::CallBase* call : deleteDestroyingCalls)
        {
            llvm::IRBuilder<> builder(call);
            llvm::Value* ptrArg = call->getArgOperand(0);
            if (ptrArg->getType() != voidPtrTy)
            {
                ptrArg = builder.CreateBitCast(ptrArg, voidPtrTy);
            }
            (void)replaceCall(call, ctDeleteDestroying, {ptrArg});
        }

        for (llvm::CallBase* call : deleteArrayDestroyingCalls)
        {
            llvm::IRBuilder<> builder(call);
            llvm::Value* ptrArg = call->getArgOperand(0);
            if (ptrArg->getType() != voidPtrTy)
            {
                ptrArg = builder.CreateBitCast(ptrArg, voidPtrTy);
            }
            (void)replaceCall(call, ctDeleteArrayDestroying, {ptrArg});
        }
    }

} // namespace compilerlib
