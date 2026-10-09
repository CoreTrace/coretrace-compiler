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

CT_NOINSTR void ct_log_alloc_details(const char* label, const char* status, size_t req_size,
                                     size_t real_size, void* ptr, const char* site, CTColor color,
                                     CTLevel lvl)
{
    ct_log(lvl, "{}{}{} :: tid={} site={}\n", ct_color(color), label, ct_color(CTColor::Reset),
           ct_thread_id(), ct_site_name(site));
    ct_log(lvl, "┌-----------------------------------┐\n");
    ct_log(lvl, "| {:<16} : {:<14} |\n", "status", status);
    ct_log(lvl, "| {:<16} : {:<14} |\n", "req_size", req_size);
    ct_log(lvl, "| {:<16} : {:<14} |\n", "total_alloc_size", real_size);
    ct_log(lvl, "| {:<16} : {:<14} |\n", "ptr", std::format("{:p}", ptr));
    ct_log(lvl, "└-----------------------------------┘\n");
}
