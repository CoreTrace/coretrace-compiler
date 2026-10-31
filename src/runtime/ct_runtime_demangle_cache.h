// SPDX-License-Identifier: Apache-2.0
//
// Demangled names, decoded once: the vtable boxes name the dynamic type of every object they
// show and the function every virtual call reaches, and decoded the same names again at every
// call (#185).
//
// A name is looked up by its text, not by its address: a library unloaded and another loaded
// in its place may put another name at the same address, which then gets its own result.
// The cache holds at most kMaxEntries names; past that, a new name is decoded every time. A name
// its demangler could not decode for now is not kept, and is decoded again next time (#208).
// Released, the cache frees the names it holds and decodes every name from then on: on Windows,
// the copy of the runtime in a DLL releases its cache when FreeLibrary unloads the DLL (#227).
// Safe for concurrent use: every cache shares one lock, which fork() also takes.
#ifndef CT_RUNTIME_DEMANGLE_CACHE_H
#define CT_RUNTIME_DEMANGLE_CACHE_H

#include "ct_runtime_helpers.h"
#include "ct_runtime_internal.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

class CtDemangleCache
{
  public:
    using Demangler = bool (*)(const char* name, std::string& out);
    // A demangler that can also find itself unable to decode a name for now.
    using OutcomeDemangler = CtDemangleOutcome (*)(const char* name, std::string& out);
    static constexpr size_t kMaxEntries = 4096;

    CT_NOINSTR explicit CtDemangleCache(Demangler demangler);
    CT_NOINSTR explicit CtDemangleCache(OutcomeDemangler demangler);

    // As demangler(name, out): true, with the decoded name in `out`, or false, `out` left as
    // it was.
    CT_NODISCARD CT_NOINSTR bool demangle(const char* name, std::string& out);
    // Names held.
    CT_NODISCARD CT_NOINSTR size_t size() const;
    // Frees the names held, and keeps none from then on.
    CT_NOINSTR void release();

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

    // One of the two is set.
    Demangler demangler_ = nullptr;
    OutcomeDemangler outcome_demangler_ = nullptr;
    // None once released.
    std::optional<std::unordered_map<std::string, Result, Hash, std::equal_to<>>> names_{
        std::in_place};
};

// The lock of every cache, for the fork() handlers: nothing else may take it.
CT_NOINSTR void ct_demangle_cache_lock_acquire(void);
CT_NOINSTR void ct_demangle_cache_lock_release(void);

#endif // CT_RUNTIME_DEMANGLE_CACHE_H
