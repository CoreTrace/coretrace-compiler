#include "compilerlib/instrumentation/bounds.hpp"
#include "compilerlib/instrumentation/common.hpp"
#include "compilerlib/attributes.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Casting.h>

namespace compilerlib
{
namespace
{

CT_NODISCARD llvm::Constant *createSiteString(llvm::Module &module, llvm::StringRef site)
{
    llvm::IRBuilder<> builder(module.getContext());
    auto *global = builder.CreateGlobalString(site, ".ct.site", 0, &module);
    auto *zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(module.getContext()), 0);
    llvm::Constant *indices[] = {zero, zero};

    return llvm::ConstantExpr::getInBoundsGetElementPtr(global->getValueType(), global, indices);
}

CT_NODISCARD llvm::Value *getSiteString(llvm::Module &module,
                           const llvm::Instruction &inst,
                           llvm::DenseMap<const llvm::DILocation *, llvm::Constant *> &cache,
                           llvm::Constant *&unknown)
{
    llvm::DebugLoc loc = inst.getDebugLoc();
    if (!loc)
    {
        if (!unknown)
            unknown = createSiteString(module, "<unknown>");
        return unknown;
    }

    const llvm::DILocation *di = loc.get();

    if (auto it = cache.find(di); it != cache.end())
        return it->second;

    std::string site = formatSiteString(inst);
    llvm::Constant *value = createSiteString(module, site);
    cache[di] = value;
    return value;
}

CT_NODISCARD llvm::Value *stripPointerCastsAndGEPs(llvm::Value *value)
{
    llvm::Value *current = value;
    while (current)
    {
        if (auto *cast = llvm::dyn_cast<llvm::BitCastInst>(current))
        {
            current = cast->getOperand(0);
            continue;
        }
        if (auto *cast = llvm::dyn_cast<llvm::AddrSpaceCastInst>(current))
        {
            current = cast->getOperand(0);
            continue;
        }
        if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(current))
        {
            current = gep->getPointerOperand();
            continue;
        }
        if (auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(current))
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

CT_NODISCARD llvm::Value *findSingleStoredValue(llvm::AllocaInst *alloca)
{
    llvm::Value *stored = nullptr;
    for (llvm::User *user : alloca->users())
    {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(user);
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

CT_NODISCARD llvm::Value *resolveBasePointer(llvm::Value *ptr)
{
    llvm::Value *base = stripPointerCastsAndGEPs(ptr);
    auto *load = llvm::dyn_cast<llvm::LoadInst>(base);
    if (!load)
    {
        return base ? base : ptr;
    }

    llvm::Value *loadSrc = stripPointerCastsAndGEPs(load->getPointerOperand());
    auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(loadSrc);
    if (!alloca)
    {
        return base ? base : ptr;
    }

    llvm::Value *stored = findSingleStoredValue(alloca);
    if (!stored)
    {
        return base ? base : ptr;
    }

    llvm::Value *resolved = stripPointerCastsAndGEPs(stored);
    return resolved ? resolved : ptr;
}

void emitBoundsCheck(llvm::IRBuilder<> &builder,
                     llvm::FunctionCallee checkFn,
                     llvm::Value *base,
                     llvm::Value *ptr,
                     llvm::Value *sizeVal,
                     llvm::Value *site,
                     bool isWrite,
                     llvm::Type *voidPtrTy,
                     llvm::Type *intTy)
{
    llvm::Value *baseCast = base;
    llvm::Value *ptrCast = ptr;

    if (baseCast->getType() != voidPtrTy)
    {
        baseCast = builder.CreateBitCast(baseCast, voidPtrTy);
    }
    if (ptrCast->getType() != voidPtrTy)
    {
        ptrCast = builder.CreateBitCast(ptrCast, voidPtrTy);
    }
    llvm::Value *writeVal = llvm::ConstantInt::get(intTy, isWrite ? 1 : 0);
    builder.CreateCall(checkFn, {baseCast, ptrCast, sizeVal, site, writeVal});
}

} // namespace

void instrumentMemoryAccesses(llvm::Module &module)
{
    llvm::LLVMContext &context = module.getContext();
    const llvm::DataLayout &layout = module.getDataLayout();
    llvm::Type *voidPtrTy = llvm::PointerType::get(llvm::Type::getInt8Ty(context), 0);
    llvm::Type *sizeTy = layout.getIntPtrType(context);
    llvm::Type *intTy = llvm::Type::getInt32Ty(context);

    auto *checkTy = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                           {voidPtrTy, voidPtrTy, sizeTy, voidPtrTy, intTy},
                                           false);
    llvm::FunctionCallee checkFn = module.getOrInsertFunction("__ct_check_bounds", checkTy);

    llvm::DenseMap<const llvm::DILocation *, llvm::Constant *> siteCache;
    llvm::Constant *unknownSite = nullptr;
    llvm::SmallVector<llvm::Instruction *, 128> worklist;

    for (llvm::Function &func : module)
    {
        if (!shouldInstrument(func))
        {
            continue;
        }
        for (llvm::BasicBlock &bb : func)
        {
            for (llvm::Instruction &inst : bb)
            {
                if (llvm::isa<llvm::LoadInst>(&inst) ||
                    llvm::isa<llvm::StoreInst>(&inst) ||
                    llvm::isa<llvm::AtomicRMWInst>(&inst) ||
                    llvm::isa<llvm::AtomicCmpXchgInst>(&inst) ||
                    llvm::isa<llvm::MemIntrinsic>(&inst))
                {
                    worklist.push_back(&inst);
                }
            }
        }
    }

    for (llvm::Instruction *inst : worklist)
    {
        llvm::IRBuilder<> builder(inst);
        llvm::Value *site = getSiteString(module, *inst, siteCache, unknownSite);

        if (auto *load = llvm::dyn_cast<llvm::LoadInst>(inst))
        {
            llvm::Value *ptr = load->getPointerOperand();
            llvm::Value *base = resolveBasePointer(ptr);
            size_t size = layout.getTypeStoreSize(load->getType());
            llvm::Value *sizeVal = llvm::ConstantInt::get(sizeTy, size);
            emitBoundsCheck(builder, checkFn, base, ptr, sizeVal, site, false, voidPtrTy, intTy);
            continue;
        }
        if (auto *store = llvm::dyn_cast<llvm::StoreInst>(inst))
        {
            llvm::Value *ptr = store->getPointerOperand();
            llvm::Value *base = resolveBasePointer(ptr);
            size_t size = layout.getTypeStoreSize(store->getValueOperand()->getType());
            llvm::Value *sizeVal = llvm::ConstantInt::get(sizeTy, size);
            emitBoundsCheck(builder, checkFn, base, ptr, sizeVal, site, true, voidPtrTy, intTy);
            continue;
        }
        if (auto *atomic = llvm::dyn_cast<llvm::AtomicRMWInst>(inst))
        {
            llvm::Value *ptr = atomic->getPointerOperand();
            llvm::Value *base = resolveBasePointer(ptr);
            size_t size = layout.getTypeStoreSize(atomic->getValOperand()->getType());
            llvm::Value *sizeVal = llvm::ConstantInt::get(sizeTy, size);
            emitBoundsCheck(builder, checkFn, base, ptr, sizeVal, site, true, voidPtrTy, intTy);
            continue;
        }
        if (auto *cmpx = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(inst))
        {
            llvm::Value *ptr = cmpx->getPointerOperand();
            llvm::Value *base = resolveBasePointer(ptr);
            size_t size = layout.getTypeStoreSize(cmpx->getCompareOperand()->getType());
            llvm::Value *sizeVal = llvm::ConstantInt::get(sizeTy, size);
            emitBoundsCheck(builder, checkFn, base, ptr, sizeVal, site, true, voidPtrTy, intTy);
            continue;
        }
        if (auto *mem = llvm::dyn_cast<llvm::MemIntrinsic>(inst))
        {
            llvm::Value *len = mem->getLength();
            if (auto *constLen = llvm::dyn_cast<llvm::ConstantInt>(len))
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

            if (auto *memSet = llvm::dyn_cast<llvm::MemSetInst>(mem))
            {
                llvm::Value *ptr = memSet->getDest();
                llvm::Value *base = resolveBasePointer(ptr);
                emitBoundsCheck(builder, checkFn, base, ptr, len, site, true, voidPtrTy, intTy);
                continue;
            }

            if (auto *memTransfer = llvm::dyn_cast<llvm::MemTransferInst>(mem))
            {
                llvm::Value *dest = memTransfer->getDest();
                llvm::Value *src = memTransfer->getSource();
                llvm::Value *destBase = resolveBasePointer(dest);
                llvm::Value *srcBase = resolveBasePointer(src);
                emitBoundsCheck(builder, checkFn, destBase, dest, len, site, true, voidPtrTy, intTy);
                emitBoundsCheck(builder, checkFn, srcBase, src, len, site, false, voidPtrTy, intTy);
                continue;
            }
        }
    }
}

} // namespace compilerlib
