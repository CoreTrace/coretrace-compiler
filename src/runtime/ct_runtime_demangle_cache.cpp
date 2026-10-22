// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_demangle_cache.h"

#if !defined(_WIN32)
#include <pthread.h>
#endif

namespace
{
#if defined(_WIN32)
    SRWLOCK ct_demangle_cache_lock = SRWLOCK_INIT;
#else
    // Never destroyed: code that runs at exit still prints boxes.
    pthread_mutex_t ct_demangle_cache_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

    struct CtDemangleCacheLock
    {
        CT_NOINSTR CtDemangleCacheLock()
        {
            ct_demangle_cache_lock_acquire();
        }
        CT_NOINSTR ~CtDemangleCacheLock()
        {
            ct_demangle_cache_lock_release();
        }
        CtDemangleCacheLock(const CtDemangleCacheLock&) = delete;
        CtDemangleCacheLock& operator=(const CtDemangleCacheLock&) = delete;
    };
} // namespace

CT_NOINSTR void ct_demangle_cache_lock_acquire(void)
{
#if defined(_WIN32)
    AcquireSRWLockExclusive(&ct_demangle_cache_lock);
#else
    pthread_mutex_lock(&ct_demangle_cache_lock);
#endif
}

CT_NOINSTR void ct_demangle_cache_lock_release(void)
{
#if defined(_WIN32)
    ReleaseSRWLockExclusive(&ct_demangle_cache_lock);
#else
    pthread_mutex_unlock(&ct_demangle_cache_lock);
#endif
}

CT_NOINSTR CtDemangleCache::CtDemangleCache(Demangler demangler) : demangler_(demangler) {}

CT_NOINSTR CtDemangleCache::CtDemangleCache(OutcomeDemangler demangler)
    : outcome_demangler_(demangler)
{
}

CT_NODISCARD CT_NOINSTR bool CtDemangleCache::demangle(const char* name, std::string& out)
{
    if (!name)
    {
        return false;
    }
    CtRuntimeScope scope;
    const std::string_view text(name);
    {
        CtDemangleCacheLock lock;
        const auto it = names_.find(text);
        if (it != names_.end())
        {
            if (it->second.decoded)
            {
                out = it->second.text;
            }
            return it->second.decoded;
        }
    }

    // Decoded without the lock, which the demangler may take long to give back.
    Result result{false, {}};
    const CtDemangleOutcome outcome = outcome_demangler_ ? outcome_demangler_(name, result.text)
                                      : demangler_(name, result.text)
                                          ? CtDemangleOutcome::Decoded
                                          : CtDemangleOutcome::NotDecodable;
    if (outcome == CtDemangleOutcome::Unavailable)
    {
        return false;
    }
    const bool decoded = outcome == CtDemangleOutcome::Decoded;
    result.decoded = decoded;
    if (decoded)
    {
        out = result.text;
    }
    try
    {
        CtDemangleCacheLock lock;
        if (names_.size() < kMaxEntries)
        {
            names_.emplace(std::string(text), std::move(result));
        }
    }
    catch (...)
    {
        // No memory to keep it: the name is decoded again next time.
    }
    return decoded;
}

CT_NODISCARD CT_NOINSTR size_t CtDemangleCache::size() const
{
    CtDemangleCacheLock lock;
    return names_.size();
}
