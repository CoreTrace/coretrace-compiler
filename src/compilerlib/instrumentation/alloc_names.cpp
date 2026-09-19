// SPDX-License-Identifier: Apache-2.0
#include "alloc_internal.hpp"

#include "coretrace/runtime_abi.h"

namespace compilerlib
{
    int escapeRank(EscapeState state)
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

    const char* escapeStateName(EscapeState state)
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

    llvm::StringRef normalizeSymbolName(llvm::StringRef name)
    {
        // LLVM uses the '\01' prefix to mark asm labels (e.g. @"\01_mmap").
        if (!name.empty() && name.front() == '\01')
        {
            return name.drop_front();
        }
        return name;
    }

    bool isMmapLikeName(llvm::StringRef name)
    {
        name = normalizeSymbolName(name);
        return name == "mmap" || name == "_mmap" || name == "__mmap" || name.starts_with("mmap$") ||
               name.starts_with("_mmap$") || name.starts_with("__mmap$");
    }

    bool isMunmapLikeName(llvm::StringRef name)
    {
        name = normalizeSymbolName(name);
        return name == "munmap" || name == "_munmap" || name == "__munmap" ||
               name.starts_with("munmap$") || name.starts_with("_munmap$") ||
               name.starts_with("__munmap$");
    }

    bool isBrkLikeName(llvm::StringRef name)
    {
        name = normalizeSymbolName(name);
        return name == "brk" || name == "_brk" || name == "__brk" || name.starts_with("brk$") ||
               name.starts_with("_brk$") || name.starts_with("__brk$");
    }

    bool isSbrkLikeName(llvm::StringRef name)
    {
        name = normalizeSymbolName(name);
        return name == "sbrk" || name == "_sbrk" || name == "__sbrk" || name.starts_with("sbrk$") ||
               name.starts_with("_sbrk$") || name.starts_with("__sbrk$");
    }

    bool isOperatorNewName(llvm::StringRef name, bool& isArray, OperatorNewKind& kind)
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

    bool isOperatorDeleteName(llvm::StringRef name, bool& isArray, OperatorDeleteKind& kind)
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
            name == "__ZdlPvmRKSt9nothrow_t" || name == "_ZdlPvmSt11align_val_tRKSt9nothrow_t" ||
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
            name == "__ZdaPvmRKSt9nothrow_t" || name == "_ZdaPvmSt11align_val_tRKSt9nothrow_t" ||
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

    bool isFreeLikeName(llvm::StringRef name)
    {
        return name == "free" || name == CT_RUNTIME_SYMBOL(__ct_free) ||
               name == CT_RUNTIME_SYMBOL(__ct_autofree) || name == CT_RUNTIME_SYMBOL(__ct_delete) ||
               name == CT_RUNTIME_SYMBOL(__ct_delete_array) ||
               name == CT_RUNTIME_SYMBOL(__ct_delete_nothrow) ||
               name == CT_RUNTIME_SYMBOL(__ct_delete_array_nothrow) ||
               name == CT_RUNTIME_SYMBOL(__ct_autofree_delete) ||
               name == CT_RUNTIME_SYMBOL(__ct_autofree_delete_array) ||
               name == CT_RUNTIME_SYMBOL(__ct_munmap) ||
               name == CT_RUNTIME_SYMBOL(__ct_autofree_munmap);
    }

} // namespace compilerlib
