// SPDX-License-Identifier: Apache-2.0
//
// Demangled names, decoded once: the vtable boxes name the dynamic type of every object they
// show and the function every virtual call reaches, and decoded the same names again at every
// call (#185).
//
// A name is looked up by its text, not by its address: a library unloaded and another loaded
// in its place may put another name at the same address, which then gets its own result.
// The cache holds at most kMaxEntries names; past that, a new name is decoded every time.
// Safe for concurrent use: every cache shares one lock, which fork() also takes.
#ifndef CT_RUNTIME_DEMANGLE_CACHE_H
#define CT_RUNTIME_DEMANGLE_CACHE_H

#include "ct_runtime_internal.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

class CtDemangleCache
{
  public:
    using Demangler = bool (*)(const char* name, std::string& out);
    static constexpr size_t kMaxEntries = 4096;

    CT_NOINSTR explicit CtDemangleCache(Demangler demangler);

    // As demangler(name, out): true, with the decoded name in `out`, or false, `out` left as
    // it was.
    CT_NODISCARD CT_NOINSTR bool demangle(const char* name, std::string& out);
    // Names held.
    CT_NODISCARD CT_NOINSTR size_t size() const;

  private:
    struct Hash
    {
        using is_transparent = void;
        CT_NOINSTR size_t operator()(std::string_view text) const
        {
            return std::hash<std::string_view>{}(text);
        }
    };
    struct Result
    {
        bool decoded;
        std::string text;
    };

    Demangler demangler_;
    std::unordered_map<std::string, Result, Hash, std::equal_to<>> names_;
};

// The lock of every cache, for the fork() handlers: nothing else may take it.
CT_NOINSTR void ct_demangle_cache_lock_acquire(void);
CT_NOINSTR void ct_demangle_cache_lock_release(void);

#endif // CT_RUNTIME_DEMANGLE_CACHE_H
