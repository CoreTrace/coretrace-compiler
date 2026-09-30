// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_internal.h"

#if !defined(_WIN32)
#include <pthread.h>

namespace
{
    // Never destroyed: code that runs at exit still logs.
    pthread_mutex_t ct_log_mutex = PTHREAD_MUTEX_INITIALIZER;
} // namespace

CT_NOINSTR void ct_log_lock_acquire(void)
{
    pthread_mutex_lock(&ct_log_mutex);
}

CT_NOINSTR void ct_log_lock_release(void)
{
    pthread_mutex_unlock(&ct_log_mutex);
}
#else
// Windows has no fork(): nothing waits on this lock there.
CT_NOINSTR void ct_log_lock_acquire(void) {}

CT_NOINSTR void ct_log_lock_release(void) {}
#endif

// #############################################
//  Runtime-specific string utilities
//  These are NOT part of coretrace-logger because
//  they are specific to the instrumentation runtime.
// #############################################

CT_NODISCARD CT_NOINSTR size_t ct_strlen(const char* str)
{
    size_t len = 0;

    if (!str)
        return 0;

    while (str[len] != '\0')
        ++len;

    return len;
}

CT_NODISCARD CT_NOINSTR int ct_streq(const char* lhs, const char* rhs)
{
    if (!lhs || !rhs)
        return 0;

    while (*lhs != '\0' && *rhs != '\0')
    {
        if (*lhs != *rhs)
            return 0;
        ++lhs;
        ++rhs;
    }
    return *lhs == *rhs;
}

CT_NODISCARD CT_NOINSTR const char* ct_site_name(const char* site)
{
    if (site && site[0] != '\0')
        return site;

    if (ct_current_site && ct_current_site[0] != '\0')
    {
        return ct_current_site;
    }
    return "<unknown>";
}
