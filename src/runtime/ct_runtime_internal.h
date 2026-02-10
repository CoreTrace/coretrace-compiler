#ifndef CT_RUNTIME_INTERNAL_H
#define CT_RUNTIME_INTERNAL_H

#include "compilerlib/attributes.hpp"

#include <coretrace/logger.hpp>

#include <cstddef>
#include <cstdint>
#include <errno.h>
#include <format>
#include <string>
#include <string_view>
#include <unistd.h>

#define CT_NOINSTR __attribute__((no_instrument_function))

// #############################################
//  Compatibility aliases: CTColor -> coretrace::Color
// #############################################

using CTColor = coretrace::Color;
using CTLevel = coretrace::Level;

// #############################################
//  Entry states (runtime-specific, not in logger)
// #############################################

enum
{
    CT_ENTRY_EMPTY = 0,
    CT_ENTRY_USED = 1,
    CT_ENTRY_TOMB = 2,
    CT_ENTRY_FREED = 3,
    CT_ENTRY_AUTOFREED = 4
};

// #############################################
//  Runtime globals
// #############################################

extern int ct_disable_trace;
extern int ct_disable_alloc;
extern int ct_disable_bounds;
extern int ct_bounds_abort;
extern int ct_shadow_enabled;
extern int ct_shadow_aggressive;
extern int ct_autofree_enabled;
extern int ct_alloc_trace_enabled;
extern int ct_vtable_diag_enabled;
extern int ct_alloc_disabled_by_config;
extern int ct_alloc_disabled_by_env;
extern int ct_early_trace;
extern size_t ct_early_trace_count;
extern size_t ct_early_trace_limit;
extern thread_local const char* ct_current_site;

// Feature flags (C API friendly).
#define CT_FEATURE_TRACE (1ull << 0)
#define CT_FEATURE_ALLOC (1ull << 1)
#define CT_FEATURE_BOUNDS (1ull << 2)
#define CT_FEATURE_SHADOW (1ull << 3)
#define CT_FEATURE_SHADOW_AGGR (1ull << 4)
#define CT_FEATURE_AUTOFREE (1ull << 5)
#define CT_FEATURE_ALLOC_TRACE (1ull << 6)
#define CT_FEATURE_VTABLE_DIAG (1ull << 7)
#define CT_FEATURE_EARLY_TRACE (1ull << 8)

extern "C"
{
    CT_NODISCARD CT_NOINSTR int ct_is_enabled(uint64_t feature);
    CT_NOINSTR void ct_set_enabled(uint64_t feature, int enabled);
    CT_NODISCARD CT_NOINSTR uint64_t ct_get_features(void);

    CT_NODISCARD CT_NOINSTR int ct_bounds_abort_enabled(void);
    CT_NOINSTR void ct_set_bounds_abort(int enabled);

    CT_NODISCARD CT_NOINSTR int ct_early_trace_should_log(void);
}

// #############################################
//  Runtime-specific functions (NOT in coretrace-logger)
// #############################################

CT_NODISCARD CT_NOINSTR size_t ct_strlen(const char* str);
CT_NODISCARD CT_NOINSTR int ct_streq(const char* lhs, const char* rhs);
CT_NODISCARD CT_NOINSTR const char* ct_site_name(const char* site);
CT_NOINSTR void ct_maybe_install_backtrace(void);
CT_NOINSTR void ct_init_env_once(void);
CT_NOINSTR void ct_lock_acquire(void);
CT_NOINSTR void ct_lock_release(void);
CT_NODISCARD CT_NOINSTR int ct_table_insert(void* ptr, size_t req_size, size_t size,
                                            const char* site, unsigned char kind);
CT_NODISCARD CT_NOINSTR int ct_table_remove(void* ptr, size_t* size_out, size_t* req_size_out,
                                            const char** site_out);
CT_NODISCARD CT_NOINSTR int ct_table_lookup(const void* ptr, size_t* size_out, size_t* req_size_out,
                                            const char** site_out, unsigned char* state_out);
CT_NODISCARD CT_NOINSTR int ct_table_lookup_containing(const void* ptr, void** base_out,
                                                       size_t* size_out, size_t* req_size_out,
                                                       const char** site_out,
                                                       unsigned char* state_out);
CT_NOINSTR void ct_shadow_poison_range(const void* addr, size_t size);
CT_NOINSTR void ct_shadow_unpoison_range(const void* addr, size_t size);
CT_NODISCARD CT_NOINSTR int ct_shadow_check_access(const void* ptr, size_t access_size,
                                                   const void* base, size_t req_size,
                                                   size_t alloc_size, const char* alloc_site,
                                                   const char* site, int is_write,
                                                   unsigned char state);
CT_NOINSTR void ct_report_bounds_error(const void* base, const void* ptr, size_t access_size,
                                       const char* site, int is_write, size_t req_size,
                                       size_t alloc_size, const char* alloc_site,
                                       unsigned char state);

// #############################################
//  Compatibility wrappers: delegate to coretrace::*
//  These allow existing runtime code to keep using
//  the ct_* API without any changes.
// #############################################

CT_NODISCARD CT_NOINSTR inline std::string_view ct_color(CTColor color)
{
    return coretrace::color(color);
}

CT_NODISCARD CT_NOINSTR inline std::string_view ct_level_label(CTLevel level)
{
    return coretrace::level_label(level);
}

CT_NODISCARD CT_NOINSTR inline std::string_view ct_level_color(CTLevel level)
{
    return coretrace::level_color(level);
}

CT_NODISCARD CT_NOINSTR inline int ct_pid(void)
{
    return coretrace::pid();
}

CT_NODISCARD CT_NOINSTR inline unsigned long long ct_thread_id(void)
{
    return coretrace::thread_id();
}

CT_NODISCARD CT_NOINSTR inline int ct_log_is_enabled(void)
{
    return coretrace::log_is_enabled() ? 1 : 0;
}

CT_NOINSTR inline void ct_enable_logging(void)
{
    coretrace::enable_logging();
}

CT_NOINSTR inline void ct_disable_logging(void)
{
    coretrace::disable_logging();
}

CT_NOINSTR inline void ct_write_raw(const char* data, size_t size)
{
    coretrace::write_raw(data, size);
}

CT_NOINSTR inline void ct_write_str(std::string_view str)
{
    coretrace::write_str(str);
}

CT_NOINSTR inline void ct_write_cstr(const char* str)
{
    if (!str)
        return;
    coretrace::write_str(std::string_view(str));
}

CT_NOINSTR inline void ct_write_dec(size_t value)
{
    coretrace::write_dec(value);
}

CT_NOINSTR inline void ct_write_hex(uintptr_t value)
{
    coretrace::write_hex(value);
}

CT_NOINSTR inline void ct_write_prefix(CTLevel level)
{
    coretrace::write_prefix(level);
}

// #############################################
//  ct_log: compatibility wrapper
//  Delegates to coretrace::write_log_line for
//  mutex-protected atomic output.
// #############################################

template <typename... Args>
CT_NOINSTR inline void ct_log(CTLevel level, std::string_view fmt, Args&&... args)
{
    if (!coretrace::log_is_enabled())
    {
        return;
    }
    try
    {
        std::string msg = std::vformat(fmt, std::make_format_args(args...));
        if (msg.empty())
        {
            return;
        }

        coretrace::write_log_line(level, {}, msg, std::source_location::current());
    }
    catch (...)
    {
        static const char fallback[] = "ct: log format error\n";
        coretrace::write_raw(fallback, sizeof(fallback) - 1);
    }
}

#endif // CT_RUNTIME_INTERNAL_H
