// SPDX-License-Identifier: Apache-2.0
//
// Derives the LLVM declaration of a runtime entry point from its C prototype in
// coretrace/runtime_abi.h, so the passes never spell a name or a signature by hand.
// Only the prototype's type is used: compilerlib does not link the runtime.
#ifndef COMPILERLIB_INSTRUMENTATION_RUNTIME_ABI_HPP
#define COMPILERLIB_INSTRUMENTATION_RUNTIME_ABI_HPP

#include "coretrace/runtime_abi.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <cstddef>
#include <type_traits>

namespace compilerlib
{
    namespace detail
    {
        // C parameter/return type -> IR type on the module's target.
        template <typename T> llvm::Type* runtimeIrType(llvm::Module& module)
        {
            llvm::LLVMContext& context = module.getContext();
            if constexpr (std::is_void_v<T>)
                return llvm::Type::getVoidTy(context);
            else if constexpr (std::is_pointer_v<T>)
                return llvm::PointerType::get(context, 0);
            else if constexpr (std::is_same_v<T, size_t>)
                return module.getDataLayout().getIntPtrType(context);
            else if constexpr (std::is_same_v<T, long long>)
                return llvm::Type::getInt64Ty(context);
            else if constexpr (std::is_same_v<T, int>)
                return llvm::Type::getInt32Ty(context);
            else if constexpr (std::is_same_v<T, double>)
                return llvm::Type::getDoubleTy(context);
            else
                static_assert(sizeof(T) == 0, "unsupported type in the runtime ABI");
        }

        template <typename F> struct RuntimeSignature;

        template <typename R, typename... A> struct RuntimeSignature<R (*)(A...)>
        {
            static llvm::FunctionType* get(llvm::Module& module)
            {
                return llvm::FunctionType::get(runtimeIrType<R>(module),
                                               {runtimeIrType<A>(module)...}, false);
            }
        };
    } // namespace detail
} // namespace compilerlib

// Declares (or fetches) runtime entry point `sym` in `module` with its ABI type.
#define CT_RUNTIME_CALLEE(module, sym)                                                             \
    (module).getOrInsertFunction(                                                                  \
        #sym, ::compilerlib::detail::RuntimeSignature<decltype(&(sym))>::get(module))

#endif // COMPILERLIB_INSTRUMENTATION_RUNTIME_ABI_HPP
