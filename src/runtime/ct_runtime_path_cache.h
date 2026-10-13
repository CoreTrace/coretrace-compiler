// SPDX-License-Identifier: Apache-2.0
//
// Resolved module paths, for the vtable diagnostics, which tell whether a vtable and the
// function a virtual call reaches belong to the same module (#185). realpath() walks the file
// system component by component, and ran twice per virtual call.
//
// An entry holds a path's text, the identity (device and inode) of the file the path led to
// when it was resolved, and the resolved path. It answers only while the path still leads to
// that file, which a stat() of the path checks at every lookup: when a symbolic link on the
// path changes target, or the file at the path is replaced, as when a library is rebuilt or
// another loaded through the same path, the path is resolved again. A file with several hard
// links is not kept, since two paths to it may resolve to different texts. Nor is a result
// when the resolved path no longer leads to that file, the path having changed in between.
//
// When the path leads to no file, realpath() would fail too: the lookup fails without it. At
// most kMaxEntries paths are kept; past that, a new path is resolved every time. Safe for
// concurrent use, under one lock that the fork() handlers also take. POSIX only.
#ifndef CT_RUNTIME_PATH_CACHE_H
#define CT_RUNTIME_PATH_CACHE_H

#include "ct_runtime_internal.h"

#include <sys/types.h>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

// realpath() into a string: false, `out` left as it was, when it fails.
CT_NODISCARD CT_NOINSTR bool ct_realpath(const char* path, std::string& out);

class CtPathCache
{
  public:
    using Resolver = bool (*)(const char* path, std::string& out);
    static constexpr size_t kMaxEntries = 1024;

    CT_NOINSTR explicit CtPathCache(Resolver resolver = ct_realpath);

    // As resolver(path, out), for the file `path` leads to now: true, with the resolved path
    // in `out`, or false, `out` left as it was.
    CT_NODISCARD CT_NOINSTR bool resolve(const char* path, std::string& out);
    // Paths held.
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
    struct Entry
    {
        dev_t device;
        ino_t inode;
        std::string resolved;
    };

    Resolver resolver_;
    std::unordered_map<std::string, Entry, Hash, std::equal_to<>> paths_;
};

// The lock of every cache, for the fork() handlers: nothing else may take it.
CT_NOINSTR void ct_path_cache_lock_acquire(void);
CT_NOINSTR void ct_path_cache_lock_release(void);

#endif // CT_RUNTIME_PATH_CACHE_H
