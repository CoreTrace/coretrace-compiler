// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_internal.h"

#if defined(_WIN32)
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#else
#include <poll.h>
#include <pthread.h>
#endif

#if !defined(_WIN32)
namespace
{
    // Never destroyed: code that runs at exit still logs.
    pthread_mutex_t ct_log_mutex = PTHREAD_MUTEX_INITIALIZER;
} // namespace

CT_NOINSTR void ct_log_lock_acquire(void)
{
    // A thread that forks holds it already: see ct_runtime_locked_for_fork.
    if (!ct_runtime_locked_for_fork())
    {
        pthread_mutex_lock(&ct_log_mutex);
    }
}

CT_NOINSTR void ct_log_lock_release(void)
{
    if (!ct_runtime_locked_for_fork())
    {
        pthread_mutex_unlock(&ct_log_mutex);
    }
}
#else
// Windows has no fork(): nothing waits on this lock there.
CT_NOINSTR void ct_log_lock_acquire(void) {}

CT_NOINSTR void ct_log_lock_release(void) {}
#endif

namespace
{
    // The buffer of the outermost CtLogBlock of this thread, if any. A raw pointer, with
    // nothing to destroy when the thread ends.
    thread_local std::string* ct_log_block_buffer = nullptr;
    // What the logger's output on this thread does when stderr is full.
    thread_local CtStderrFull ct_log_when_stderr_full = CtStderrFull::Wait;
} // namespace

CT_NOINSTR void ct_write_stderr(const char* data, size_t size, CtStderrFull when_full)
{
#if defined(_WIN32)
    // _write blocks on a full pipe: there is no non-blocking stderr to give up on.
    (void)when_full;
    const int fd = _fileno(stderr);
    while (size > 0)
    {
        const size_t chunk = std::min(size, static_cast<size_t>((std::numeric_limits<int>::max)()));
        const int written = _write(fd, data, static_cast<unsigned int>(chunk));
        if (written <= 0)
        {
            return;
        }
        data += written;
        size -= static_cast<size_t>(written);
    }
#else
    while (size > 0)
    {
        const ssize_t written = write(STDERR_FILENO, data, size);
        if (written > 0)
        {
            data += written;
            size -= static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
        {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            when_full == CtStderrFull::Wait)
        {
            // A non-blocking descriptor that is full: wait until it takes more.
            pollfd ready{STDERR_FILENO, POLLOUT, 0};
            if (poll(&ready, 1, -1) >= 0 || errno == EINTR)
            {
                continue;
            }
        }
        return;
    }
#endif
}

CT_NOINSTR void ct_log_sink(const char* data, size_t size)
{
    std::string* buffer = ct_log_block_buffer;
    if (buffer)
    {
        try
        {
            buffer->append(data, size);
            return;
        }
        catch (...)
        {
            // No memory for the block: what it holds goes out first, then this, in order, and
            // in one piece, as a block would.
            CtLogLockGuard lock;
            ct_write_stderr(buffer->data(), buffer->size(), ct_log_when_stderr_full);
            buffer->clear();
            ct_write_stderr(data, size, ct_log_when_stderr_full);
            return;
        }
    }
    // Outside any block. On POSIX, only the crash handler's report comes here: it must not wait
    // for the log lock, which the thread it interrupted may hold; every other writer opens a
    // CtLogBlock. On Windows, where the log lock does nothing, the leak and crash reports too.
    ct_write_stderr(data, size, ct_log_when_stderr_full);
}

CT_NOINSTR CtLogBlock::CtLogBlock() : outermost_(ct_log_block_buffer == nullptr)
{
    if (outermost_)
    {
        ct_log_block_buffer = &buffer_;
    }
}

CT_NOINSTR CtLogBlock::~CtLogBlock()
{
    if (!outermost_ || ct_log_block_buffer != &buffer_)
    {
        return;
    }
    ct_log_block_buffer = nullptr;
    if (!buffer_.empty())
    {
        CtLogLockGuard lock;
        ct_write_stderr(buffer_.data(), buffer_.size());
    }
}

CT_NOINSTR void ct_log_block_abandon(void)
{
    ct_log_block_buffer = nullptr;
}

CT_NOINSTR void ct_log_stop_waiting_for_stderr(void)
{
    ct_log_when_stderr_full = CtStderrFull::GiveUp;
}

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

namespace
{
#if defined(_WIN32)
    // ASCII, as the other boxes of the Windows runtime: a console or a reader that decodes the
    // output with a legacy code page, such as cp1252, garbles UTF-8 box-drawing characters.
    constexpr const char* kBoxTop = "+-----------------------------------+\n";
    constexpr const char* kBoxBottom = "+-----------------------------------+\n";
#else
    constexpr const char* kBoxTop = "┌-----------------------------------┐\n";
    constexpr const char* kBoxBottom = "└-----------------------------------┘\n";
#endif
} // namespace

CT_NOINSTR void ct_log_alloc_details(const char* label, const char* status, size_t req_size,
                                     size_t real_size, void* ptr, const char* site, CTColor color,
                                     CTLevel lvl)
{
    CtLogBlock box;
    ct_log(lvl, "{}{}{} :: tid={} site={}\n", ct_color(color), label, ct_color(CTColor::Reset),
           ct_thread_id(), ct_site_name(site));
    ct_log(lvl, kBoxTop);
    ct_log(lvl, "| {:<16} : {:<14} |\n", "status", status);
    ct_log(lvl, "| {:<16} : {:<14} |\n", "req_size", req_size);
    ct_log(lvl, "| {:<16} : {:<14} |\n", "total_alloc_size", real_size);
    ct_log(lvl, "| {:<16} : {:<14} |\n", "ptr", std::format("{:p}", ptr));
    ct_log(lvl, kBoxBottom);
}

CT_NOINSTR void ct_log_realloc_details(const char* label, const char* status, size_t old_req_size,
                                       size_t old_real_size, void* old_ptr, size_t new_req_size,
                                       size_t new_real_size, void* new_ptr, const char* site,
                                       CTColor color)
{
    CtLogBlock box;
    ct_log(CTLevel::Warn, "{}{}{} :: tid={} site={}\n", ct_color(color), label,
           ct_color(CTColor::Reset), ct_thread_id(), ct_site_name(site));
    ct_log(CTLevel::Warn, kBoxTop);
    ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "status", status);
    ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "old_req_size", old_req_size);
    ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "new_req_size", new_req_size);
    ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "old_alloc_size", old_real_size);
    ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "new_alloc_size", new_real_size);
    ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "old_ptr", std::format("{:p}", old_ptr));
    ct_log(CTLevel::Warn, "| {:<16} : {:<14} |\n", "new_ptr", std::format("{:p}", new_ptr));
    ct_log(CTLevel::Warn, kBoxBottom);
}

CT_NOINSTR void ct_log_realloc_freed(void* ptr, const char* site)
{
    ct_log(CTLevel::Warn, "{}tracing-realloc ptr={:p} (freed) site={}{}\n", ct_color(CTColor::Red),
           ptr, ct_site_name(site), ct_color(CTColor::Reset));
}

CT_NOINSTR void ct_log_release(const char* label, void* ptr, size_t size, const char* kind,
                               const char* alloc_site)
{
    ct_log(CTLevel::Info, "{}{} ptr={:p} size={} kind={} alloc_site={}{}\n",
           ct_color(CTColor::Cyan), label, ptr, size, kind, ct_site_name(alloc_site),
           ct_color(CTColor::Reset));
}

CT_NOINSTR void ct_log_release_unknown(const char* label, void* ptr)
{
    ct_log(CTLevel::Info, "{}{} ptr={:p} (unknown){}\n", ct_color(CTColor::Cyan), label, ptr,
           ct_color(CTColor::Reset));
}

CT_NOINSTR void ct_log_release_null(const char* label)
{
    ct_log(CTLevel::Warn, "{}{} ptr=null{}\n", ct_color(CTColor::Yellow), label,
           ct_color(CTColor::Reset));
}

CT_NOINSTR void ct_log_double_free(const char* label, void* ptr, const char* site,
                                   const char* alloc_site)
{
    ct_log(CTLevel::Warn, "{}{} ptr={:p} (double free) site={} alloc_site={}{}\n",
           ct_color(CTColor::Red), label, ptr, ct_site_name(site), ct_site_name(alloc_site),
           ct_color(CTColor::Reset));
}

CT_NOINSTR void ct_log_deallocator_mismatch(const char* label, void* ptr, const char* expected,
                                            const char* recorded, const char* alloc_site)
{
    ct_log(CTLevel::Warn,
           "{}{} ptr={:p} (deallocator mismatch) expected={} recorded={} alloc_site={}{}\n",
           ct_color(CTColor::Red), label, ptr, expected, recorded, ct_site_name(alloc_site),
           ct_color(CTColor::Reset));
}

CT_NOINSTR void ct_log_autofree(void* ptr, size_t size, const char* site)
{
    ct_log(CTLevel::Warn, "{}auto-free ptr={:p} size={} site={}{}\n",
           ct_color(CTColor::BgBrightYellow), ptr, size, ct_site_name(site),
           ct_color(CTColor::Reset));
}

CT_NOINSTR void ct_log_autofree_skipped(void* ptr, const char* reason)
{
    if (!ptr)
    {
        ct_log(CTLevel::Warn, "{}ct: auto-free ptr=null{}\n", ct_color(CTColor::BgBrightYellow),
               ct_color(CTColor::Reset));
        return;
    }
    ct_log(CTLevel::Warn, "{}ct: auto-free skipped ptr={:p} ({}){}\n",
           ct_color(CTColor::BgBrightYellow), ptr, reason, ct_color(CTColor::Reset));
}
