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

    namespace
    {
        struct OperatorNewName
        {
            llvm::StringRef name;
            bool isArray;
            OperatorNewKind kind;
        };

        struct OperatorDeleteName
        {
            llvm::StringRef name;
            bool isArray;
            OperatorDeleteKind kind;
        };

        // Global operator new/delete overloads the pass rewrites, as mangled by the
        // Itanium C++ ABI (Linux, macOS) and the Microsoft C++ ABI (Windows x64 and
        // ARM64 mangle them identically).
        //
        // Aligned operator new is never rewritten: the runtime allocates through the
        // unaligned operator. Aligned operator delete is rewritten for Itanium only,
        // where aligned blocks come from aligned_alloc and any operator delete may
        // release them; the Microsoft CRT allocates them with _aligned_malloc, which
        // only the aligned operator delete may release.
        constexpr OperatorNewName kOperatorNewNames[] = {
            {"_Znwm", false, OperatorNewKind::Normal},
            {"_Znam", true, OperatorNewKind::Normal},
            {"_ZnwmRKSt9nothrow_t", false, OperatorNewKind::Nothrow},
            {"_ZnamRKSt9nothrow_t", true, OperatorNewKind::Nothrow},
            {"??2@YAPEAX_K@Z", false, OperatorNewKind::Normal},
            {"??_U@YAPEAX_K@Z", true, OperatorNewKind::Normal},
            {"??2@YAPEAX_KAEBUnothrow_t@std@@@Z", false, OperatorNewKind::Nothrow},
            {"??_U@YAPEAX_KAEBUnothrow_t@std@@@Z", true, OperatorNewKind::Nothrow},
        };

        constexpr OperatorDeleteName kOperatorDeleteNames[] = {
            // Itanium: plain, sized (m), aligned (St11align_val_t) and their combinations.
            {"_ZdlPv", false, OperatorDeleteKind::Normal},
            {"_ZdlPvm", false, OperatorDeleteKind::Normal},
            {"_ZdlPvSt11align_val_t", false, OperatorDeleteKind::Normal},
            {"_ZdlPvmSt11align_val_t", false, OperatorDeleteKind::Normal},
            {"_ZdlPvRKSt9nothrow_t", false, OperatorDeleteKind::Nothrow},
            {"_ZdlPvmRKSt9nothrow_t", false, OperatorDeleteKind::Nothrow},
            {"_ZdlPvSt11align_val_tRKSt9nothrow_t", false, OperatorDeleteKind::Nothrow},
            {"_ZdlPvmSt11align_val_tRKSt9nothrow_t", false, OperatorDeleteKind::Nothrow},
            {"_ZdlPvSt19destroying_delete_t", false, OperatorDeleteKind::Destroying},
            {"_ZdaPv", true, OperatorDeleteKind::Normal},
            {"_ZdaPvm", true, OperatorDeleteKind::Normal},
            {"_ZdaPvSt11align_val_t", true, OperatorDeleteKind::Normal},
            {"_ZdaPvmSt11align_val_t", true, OperatorDeleteKind::Normal},
            {"_ZdaPvRKSt9nothrow_t", true, OperatorDeleteKind::Nothrow},
            {"_ZdaPvmRKSt9nothrow_t", true, OperatorDeleteKind::Nothrow},
            {"_ZdaPvSt11align_val_tRKSt9nothrow_t", true, OperatorDeleteKind::Nothrow},
            {"_ZdaPvmSt11align_val_tRKSt9nothrow_t", true, OperatorDeleteKind::Nothrow},
            {"_ZdaPvSt19destroying_delete_t", true, OperatorDeleteKind::Destroying},
            // Microsoft: plain, sized (_K) and nothrow.
            {"??3@YAXPEAX@Z", false, OperatorDeleteKind::Normal},
            {"??3@YAXPEAX_K@Z", false, OperatorDeleteKind::Normal},
            {"??3@YAXPEAXAEBUnothrow_t@std@@@Z", false, OperatorDeleteKind::Nothrow},
            {"??_V@YAXPEAX@Z", true, OperatorDeleteKind::Normal},
            {"??_V@YAXPEAX_K@Z", true, OperatorDeleteKind::Normal},
            {"??_V@YAXPEAXAEBUnothrow_t@std@@@Z", true, OperatorDeleteKind::Nothrow},
        };

        template <typename Entry, size_t N>
        const Entry* findOperatorName(const Entry (&table)[N], llvm::StringRef name)
        {
            // Some toolchains prefix Itanium names with an extra underscore.
            if (name.starts_with("__Z"))
            {
                name = name.drop_front();
            }
            for (const Entry& entry : table)
            {
                if (entry.name == name)
                {
                    return &entry;
                }
            }
            return nullptr;
        }
    } // namespace

    bool isOperatorNewName(llvm::StringRef name, bool& isArray, OperatorNewKind& kind)
    {
        const OperatorNewName* entry = findOperatorName(kOperatorNewNames, name);
        if (!entry)
        {
            return false;
        }
        isArray = entry->isArray;
        kind = entry->kind;
        return true;
    }

    bool isOperatorDeleteName(llvm::StringRef name, bool& isArray, OperatorDeleteKind& kind)
    {
        const OperatorDeleteName* entry = findOperatorName(kOperatorDeleteNames, name);
        if (!entry)
        {
            return false;
        }
        isArray = entry->isArray;
        kind = entry->kind;
        return true;
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
