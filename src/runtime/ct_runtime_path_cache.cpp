// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_path_cache.h"

#include <climits>
#include <cstdlib>
#include <pthread.h>
#include <sys/stat.h>

namespace
{
    // Never destroyed: code that runs at exit still prints boxes.
    pthread_mutex_t ct_path_cache_lock = PTHREAD_MUTEX_INITIALIZER;

    struct CtPathCacheLock
    {
        CT_NOINSTR CtPathCacheLock()
        {
            ct_path_cache_lock_acquire();
        }
        CT_NOINSTR ~CtPathCacheLock()
        {
            ct_path_cache_lock_release();
        }
        CtPathCacheLock(const CtPathCacheLock&) = delete;
        CtPathCacheLock& operator=(const CtPathCacheLock&) = delete;
    };

    CT_NODISCARD CT_NOINSTR bool ct_same_file(const struct stat& lhs, const struct stat& rhs)
    {
        return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino;
    }
} // namespace

CT_NOINSTR void ct_path_cache_lock_acquire(void)
{
    pthread_mutex_lock(&ct_path_cache_lock);
}

CT_NOINSTR void ct_path_cache_lock_release(void)
{
    pthread_mutex_unlock(&ct_path_cache_lock);
}

CT_NODISCARD CT_NOINSTR bool ct_realpath(const char* path, std::string& out)
{
    char resolved[PATH_MAX];
    if (!path || !realpath(path, resolved))
    {
        return false;
    }
    out.assign(resolved);
    return true;
}

CT_NOINSTR CtPathCache::CtPathCache(Resolver resolver) : resolver_(resolver) {}

CT_NODISCARD CT_NOINSTR bool CtPathCache::resolve(const char* path, std::string& out)
{
    if (!path || path[0] == '\0')
    {
        return false;
    }
    CtRuntimeScope scope;
    // The file the path leads to now. None: realpath() would fail as well.
    struct stat seen;
    if (stat(path, &seen) != 0)
    {
        return false;
    }
    const bool keepable = seen.st_nlink == 1;
    const std::string_view text(path);
    if (keepable)
    {
        CtPathCacheLock lock;
        const auto it = paths_.find(text);
        if (it != paths_.end() && it->second.device == seen.st_dev &&
            it->second.inode == seen.st_ino)
        {
            out = it->second.resolved;
            return true;
        }
    }

    // Resolved without the lock: realpath() walks the file system.
    std::string resolved;
    if (!resolver_(path, resolved))
    {
        return false;
    }
    out = resolved;
    struct stat now;
    if (!keepable || stat(resolved.c_str(), &now) != 0 || !ct_same_file(seen, now))
    {
        return true;
    }
    try
    {
        CtPathCacheLock lock;
        const auto it = paths_.find(text);
        if (it != paths_.end())
        {
            it->second = Entry{seen.st_dev, seen.st_ino, std::move(resolved)};
        }
        else if (paths_.size() < kMaxEntries)
        {
            paths_.emplace(std::string(text), Entry{seen.st_dev, seen.st_ino, std::move(resolved)});
        }
    }
    catch (...)
    {
        // No memory to keep it: the path is resolved again next time.
    }
    return true;
}

CT_NODISCARD CT_NOINSTR size_t CtPathCache::size() const
{
    CtPathCacheLock lock;
    return paths_.size();
}
