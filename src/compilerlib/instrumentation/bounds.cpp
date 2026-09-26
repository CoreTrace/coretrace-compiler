// SPDX-License-Identifier: Apache-2.0
#include "compilerlib/instrumentation/bounds.hpp"
#include "compilerlib/instrumentation/common.hpp"
#include "compilerlib/attributes.hpp"
#include "runtime_abi.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SetVector.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Casting.h>

#include <optional>
#include <string>

namespace compilerlib
{
    namespace
    {

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
                    unknown = createSiteString(module, "<unknown>");
                return unknown;
            }

            const llvm::DILocation* di = loc.get();

            if (auto it = cache.find(di); it != cache.end())
                return it->second;

            std::string site = formatSiteString(inst);
            llvm::Constant* value = createSiteString(module, site);
            cache[di] = value;
            return value;
        }

        // The pointer an integer was computed from, when the integer is ptrtoint(P) plus or
        // minus values that do not come from another pointer; nullptr otherwise. This is
        // how container_of and offsetof code written with uintptr_t rebuilds a pointer, and
        // the rebuilt pointer is meant to stay in P's allocation.
        CT_NODISCARD llvm::Value* pointerBehindInteger(llvm::Value* value, unsigned depth = 0)
        {
            constexpr unsigned kMaxDepth = 8;
            auto* op = llvm::dyn_cast<llvm::Operator>(value);
            if (!op || depth > kMaxDepth)
            {
                return nullptr;
            }
            switch (op->getOpcode())
            {
            case llvm::Instruction::PtrToInt:
                return op->getOperand(0);
            case llvm::Instruction::Add:
            {
                llvm::Value* lhs = pointerBehindInteger(op->getOperand(0), depth + 1);
                llvm::Value* rhs = pointerBehindInteger(op->getOperand(1), depth + 1);
                if (lhs && rhs)
                {
                    return nullptr;
                }
                return lhs ? lhs : rhs;
            }
            case llvm::Instruction::Sub:
                // offset - ptrtoint(P) does not point into P.
                if (pointerBehindInteger(op->getOperand(1), depth + 1))
                {
                    return nullptr;
                }
                return pointerBehindInteger(op->getOperand(0), depth + 1);
            default:
                return nullptr;
            }
        }

        CT_NODISCARD llvm::Value* stripPointerCastsAndGEPs(llvm::Value* value)
        {
            llvm::Value* current = value;
            while (current)
            {
                // Checked before the generic constant cast below, which would otherwise
                // continue with inttoptr's integer operand.
                if (llvm::Operator::getOpcode(current) == llvm::Instruction::IntToPtr)
                {
                    llvm::Value* source =
                        pointerBehindInteger(llvm::cast<llvm::Operator>(current)->getOperand(0));
                    if (!source)
                    {
                        break;
                    }
                    current = source;
                    continue;
                }
                if (auto* cast = llvm::dyn_cast<llvm::BitCastInst>(current))
                {
                    current = cast->getOperand(0);
                    continue;
                }
                if (auto* cast = llvm::dyn_cast<llvm::AddrSpaceCastInst>(current))
                {
                    current = cast->getOperand(0);
                    continue;
                }
                if (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(current))
                {
                    current = gep->getPointerOperand();
                    continue;
                }
                if (auto* ce = llvm::dyn_cast<llvm::ConstantExpr>(current))
                {
                    if (ce->isCast())
                    {
                        current = ce->getOperand(0);
                        continue;
                    }
                    if (ce->getOpcode() == llvm::Instruction::GetElementPtr)
                    {
                        current = ce->getOperand(0);
                        continue;
                    }
                }
                break;
            }
            return current;
        }

        CT_NODISCARD llvm::Value* findSingleStoredValue(llvm::AllocaInst* alloca)
        {
            llvm::Value* stored = nullptr;
            for (llvm::User* user : alloca->users())
            {
                auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                if (!store)
                {
                    continue;
                }
                if (store->getPointerOperand() != alloca)
                {
                    continue;
                }
                if (stored)
                {
                    return nullptr;
                }
                stored = store->getValueOperand();
            }
            return stored;
        }

        CT_NODISCARD llvm::Value* resolveBasePointer(llvm::Value* ptr)
        {
            llvm::Value* base = stripPointerCastsAndGEPs(ptr);
            auto* load = llvm::dyn_cast<llvm::LoadInst>(base);
            if (!load)
            {
                return base ? base : ptr;
            }

            llvm::Value* loadSrc = stripPointerCastsAndGEPs(load->getPointerOperand());
            auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(loadSrc);
            if (!alloca)
            {
                return base ? base : ptr;
            }

            llvm::Value* stored = findSingleStoredValue(alloca);
            if (!stored)
            {
                return base ? base : ptr;
            }

            llvm::Value* resolved = stripPointerCastsAndGEPs(stored);
            return resolved ? resolved : ptr;
        }

        // Size of a stack object the runtime can register: allocated once per call, with a
        // size known at compile time. Variable-length arrays are not.
        CT_NODISCARD std::optional<uint64_t> stackObjectSize(const llvm::AllocaInst& object,
                                                             const llvm::DataLayout& layout)
        {
            if (!object.isStaticAlloca() || object.isSwiftError() || object.isUsedWithInAlloca())
            {
                return std::nullopt;
            }
            std::optional<llvm::TypeSize> size = object.getAllocationSize(layout);
            if (!size || size->isScalable())
            {
                return std::nullopt;
            }
            return size->getFixedValue();
        }

        // True when [ptr, ptr + accessSize) lies inside `object`, of `objectSize` bytes, at
        // an offset known at compile time: a check could never fail.
        CT_NODISCARD bool staysInside(const llvm::Value* ptr, uint64_t accessSize,
                                      const llvm::AllocaInst& object, uint64_t objectSize,
                                      const llvm::DataLayout& layout)
        {
            llvm::APInt offset(layout.getIndexTypeSizeInBits(ptr->getType()), 0);
            const llvm::Value* origin =
                ptr->stripAndAccumulateConstantOffsets(layout, offset, /*AllowNonInbounds=*/true);
            if (origin != &object || offset.isNegative())
            {
                return false;
            }
            const uint64_t start = offset.getZExtValue();
            return start <= objectSize && accessSize <= objectSize - start;
        }

        // The variable a stack object holds, from its debug declaration; null without full
        // debug information. LLVM 19 moved declarations from intrinsics to records, and
        // LLVM 18 renamed the intrinsic lookup.
        CT_NODISCARD const llvm::DILocalVariable* declaredVariable(llvm::AllocaInst& object)
        {
#if LLVM_VERSION_MAJOR >= 19
            if (llvm::TinyPtrVector<llvm::DbgVariableRecord*> records =
                    llvm::findDVRDeclares(&object);
                !records.empty())
            {
                return records.front()->getVariable();
            }
#endif
#if LLVM_VERSION_MAJOR >= 18
            llvm::TinyPtrVector<llvm::DbgDeclareInst*> declares = llvm::findDbgDeclares(&object);
#else
            llvm::TinyPtrVector<llvm::DbgDeclareInst*> declares = llvm::FindDbgDeclareUses(&object);
#endif
            return declares.empty() ? nullptr : declares.front()->getVariable();
        }

        // Where a stack object comes from, as "file:line": its variable's declaration when
        // the program has full debug information, its function's otherwise.
        CT_NODISCARD llvm::Value* stackObjectSite(llvm::Module& module, llvm::AllocaInst& object,
                                                  llvm::Constant*& unknown)
        {
            llvm::StringRef file;
            unsigned line = 0;
            if (const llvm::DILocalVariable* variable = declaredVariable(object))
            {
                file = variable->getFilename();
                line = variable->getLine();
            }
            else if (const llvm::DISubprogram* subprogram = object.getFunction()->getSubprogram())
            {
                file = subprogram->getFilename();
                line = subprogram->getLine();
            }
            if (file.empty())
            {
                if (!unknown)
                    unknown = createSiteString(module, "<unknown>");
                return unknown;
            }
            return createSiteString(module, file.str() + ":" + std::to_string(line));
        }

        // Registers `objects` while `func` runs: each is pushed right after its alloca, so
        // before anything can access it, wherever other passes put code in the entry
        // block, and popped before every return and resume. Every exit restores the depth
        // the first push returned, which also drops objects that frames an exception or a
        // longjmp left without returning had registered above it.
        void registerStackObjects(llvm::Function& func, llvm::ArrayRef<llvm::AllocaInst*> objects,
                                  const llvm::DataLayout& layout, llvm::Constant*& unknownSite)
        {
            if (objects.empty())
            {
                return;
            }
            // Nothing may run between a musttail call and its return.
            for (llvm::Instruction& inst : llvm::instructions(func))
            {
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                    call && call->isMustTailCall())
                {
                    return;
                }
            }

            llvm::Module& module = *func.getParent();
            llvm::Type* sizeTy = layout.getIntPtrType(module.getContext());
            llvm::FunctionCallee pushFn = CT_RUNTIME_CALLEE(module, __ct_stack_push);
            llvm::FunctionCallee popFn = CT_RUNTIME_CALLEE(module, __ct_stack_pop);

            // Registered objects are static allocas, all in the entry block: in block order,
            // the first push is the first to run.
            llvm::SmallVector<llvm::AllocaInst*, 8> ordered(objects.begin(), objects.end());
            llvm::sort(ordered, [](const llvm::AllocaInst* lhs, const llvm::AllocaInst* rhs)
                       { return lhs->comesBefore(rhs); });
            llvm::Value* depth = nullptr;
            for (llvm::AllocaInst* object : ordered)
            {
                llvm::IRBuilder<> builder(object->getNextNode());
                llvm::Value* size =
                    llvm::ConstantInt::get(sizeTy, *stackObjectSize(*object, layout));
                llvm::Value* pushed = builder.CreateCall(
                    pushFn, {object, size, stackObjectSite(module, *object, unknownSite)});
                if (!depth)
                {
                    depth = pushed;
                }
            }
            for (llvm::BasicBlock& bb : func)
            {
                llvm::Instruction* exit = bb.getTerminator();
                if (llvm::isa<llvm::ReturnInst>(exit) || llvm::isa<llvm::ResumeInst>(exit))
                {
                    llvm::IRBuilder<>(exit).CreateCall(popFn, {depth});
                }
            }
        }

        void emitBoundsCheck(llvm::IRBuilder<>& builder, llvm::FunctionCallee checkFn,
                             llvm::Value* base, llvm::Value* ptr, llvm::Value* sizeVal,
                             llvm::Value* site, bool isWrite, llvm::Type* voidPtrTy,
                             llvm::Type* intTy)
        {
            llvm::Value* baseCast = base;
            llvm::Value* ptrCast = ptr;

            if (baseCast->getType() != voidPtrTy)
            {
                baseCast = builder.CreateBitCast(baseCast, voidPtrTy);
            }
            if (ptrCast->getType() != voidPtrTy)
            {
                ptrCast = builder.CreateBitCast(ptrCast, voidPtrTy);
            }
            llvm::Value* writeVal = llvm::ConstantInt::get(intTy, isWrite ? 1 : 0);
            builder.CreateCall(checkFn, {baseCast, ptrCast, sizeVal, site, writeVal});
        }

    } // namespace

    void instrumentMemoryAccesses(llvm::Module& module)
    {
        llvm::LLVMContext& context = module.getContext();
        const llvm::DataLayout& layout = module.getDataLayout();
        llvm::Type* voidPtrTy = llvm::PointerType::get(llvm::Type::getInt8Ty(context), 0);
        llvm::Type* sizeTy = layout.getIntPtrType(context);
        llvm::Type* intTy = llvm::Type::getInt32Ty(context);

        llvm::FunctionCallee checkFn = CT_RUNTIME_CALLEE(module, __ct_check_bounds);

        llvm::DenseMap<const llvm::DILocation*, llvm::Constant*> siteCache;
        llvm::Constant* unknownSite = nullptr;

        for (llvm::Function& func : module)
        {
            if (!shouldInstrument(func))
            {
                continue;
            }

            // A stack object whose address escapes is registered: a callee may check
            // accesses against it. Decided before the checks exist, since handing an
            // object to the checker would count as an escape.
            llvm::SetVector<llvm::AllocaInst*> stackObjects;
            for (llvm::Instruction& inst : func.getEntryBlock())
            {
                auto* object = llvm::dyn_cast<llvm::AllocaInst>(&inst);
                if (object && stackObjectSize(*object, layout) &&
                    llvm::PointerMayBeCaptured(object, /*ReturnCaptures=*/true,
                                               /*StoreCaptures=*/true))
                {
                    stackObjects.insert(object);
                }
            }

            llvm::SmallVector<llvm::Instruction*, 64> accesses;
            for (llvm::Instruction& inst : llvm::instructions(func))
            {
                if (llvm::isa<llvm::LoadInst>(&inst) || llvm::isa<llvm::StoreInst>(&inst) ||
                    llvm::isa<llvm::AtomicRMWInst>(&inst) ||
                    llvm::isa<llvm::AtomicCmpXchgInst>(&inst) ||
                    llvm::isa<llvm::MemIntrinsic>(&inst))
                {
                    accesses.push_back(&inst);
                }
            }

            for (llvm::Instruction* inst : accesses)
            {
                llvm::IRBuilder<> builder(inst);

                // Checks one access, unless its base is a stack object it provably stays
                // inside; a stack object that is the base of a check gets registered.
                auto check = [&](llvm::Value* ptr, llvm::Value* sizeVal, bool isWrite)
                {
                    llvm::Value* base = resolveBasePointer(ptr);
                    if (auto* object = llvm::dyn_cast<llvm::AllocaInst>(base))
                    {
                        if (std::optional<uint64_t> objectSize = stackObjectSize(*object, layout))
                        {
                            auto* constantSize = llvm::dyn_cast<llvm::ConstantInt>(sizeVal);
                            if (constantSize && staysInside(ptr, constantSize->getZExtValue(),
                                                            *object, *objectSize, layout))
                            {
                                return;
                            }
                            stackObjects.insert(object);
                        }
                    }
                    llvm::Value* site = getSiteString(module, *inst, siteCache, unknownSite);
                    emitBoundsCheck(builder, checkFn, base, ptr, sizeVal, site, isWrite, voidPtrTy,
                                    intTy);
                };
                auto storeSize = [&](llvm::Type* type)
                { return llvm::ConstantInt::get(sizeTy, layout.getTypeStoreSize(type)); };

                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(inst))
                {
                    check(load->getPointerOperand(), storeSize(load->getType()), false);
                    continue;
                }
                if (auto* store = llvm::dyn_cast<llvm::StoreInst>(inst))
                {
                    check(store->getPointerOperand(),
                          storeSize(store->getValueOperand()->getType()), true);
                    continue;
                }
                if (auto* atomic = llvm::dyn_cast<llvm::AtomicRMWInst>(inst))
                {
                    check(atomic->getPointerOperand(),
                          storeSize(atomic->getValOperand()->getType()), true);
                    continue;
                }
                if (auto* cmpx = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(inst))
                {
                    check(cmpx->getPointerOperand(),
                          storeSize(cmpx->getCompareOperand()->getType()), true);
                    continue;
                }
                if (auto* mem = llvm::dyn_cast<llvm::MemIntrinsic>(inst))
                {
                    llvm::Value* len = mem->getLength();
                    if (auto* constLen = llvm::dyn_cast<llvm::ConstantInt>(len))
                    {
                        if (constLen->isZero())
                        {
                            continue;
                        }
                    }
                    if (len->getType() != sizeTy)
                    {
                        len = builder.CreateZExtOrTrunc(len, sizeTy);
                    }

                    if (auto* memSet = llvm::dyn_cast<llvm::MemSetInst>(mem))
                    {
                        check(memSet->getDest(), len, true);
                        continue;
                    }
                    if (auto* memTransfer = llvm::dyn_cast<llvm::MemTransferInst>(mem))
                    {
                        check(memTransfer->getDest(), len, true);
                        check(memTransfer->getSource(), len, false);
                        continue;
                    }
                }
            }

            registerStackObjects(func, stackObjects.getArrayRef(), layout, unknownSite);
        }
    }

} // namespace compilerlib
