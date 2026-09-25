// SPDX-License-Identifier: Apache-2.0
//
// Pure helpers of the allocation pass: symbol-name tables and the escape lattice.
// Internal to compilerlib (shared with the unit tests), not part of the public API.
#ifndef COMPILERLIB_INSTRUMENTATION_ALLOC_INTERNAL_HPP
#define COMPILERLIB_INSTRUMENTATION_ALLOC_INTERNAL_HPP

#include "compilerlib/attributes.hpp"

#include <llvm/ADT/StringRef.h>

namespace compilerlib
{
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

    // Ordered lattice: an allocation is freed at function exit only when its state
    // stays ReachableLocal; every other value means "do not free".
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

    CT_NODISCARD int escapeRank(EscapeState state);
    CT_NODISCARD const char* escapeStateName(EscapeState state);

    // Strips LLVM's '\01' asm-label marker (e.g. @"\01_mmap").
    CT_NODISCARD llvm::StringRef normalizeSymbolName(llvm::StringRef name);
    CT_NODISCARD bool isMmapLikeName(llvm::StringRef name);
    CT_NODISCARD bool isMunmapLikeName(llvm::StringRef name);
    CT_NODISCARD bool isBrkLikeName(llvm::StringRef name);
    CT_NODISCARD bool isSbrkLikeName(llvm::StringRef name);

    // Global operator new/delete overloads, as mangled by the Itanium (Linux, macOS)
    // and Microsoft (Windows) C++ ABIs. Aligned forms are excluded where rewriting
    // them would release memory with the wrong function; see alloc_names.cpp.
    CT_NODISCARD bool isOperatorNewName(llvm::StringRef name, bool& isArray, OperatorNewKind& kind);
    CT_NODISCARD bool isOperatorDeleteName(llvm::StringRef name, bool& isArray,
                                           OperatorDeleteKind& kind);

    // Deallocation entry points, including the runtime's own after rewriting.
    CT_NODISCARD bool isFreeLikeName(llvm::StringRef name);

    // Objective-C object allocations: the runtime functions clang emits for +alloc,
    // +allocWithZone:nil and [[X alloc] init], and the selectors of the messages it
    // sends through objc_msgSend instead, such as [X new].
    CT_NODISCARD bool isObjcAllocFunctionName(llvm::StringRef name);
    CT_NODISCARD bool isObjcAllocSelector(llvm::StringRef selector);

} // namespace compilerlib

#endif // COMPILERLIB_INSTRUMENTATION_ALLOC_INTERNAL_HPP
