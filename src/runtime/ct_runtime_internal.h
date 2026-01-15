#ifndef CT_RUNTIME_INTERNAL_H
#define CT_RUNTIME_INTERNAL_H

#include <cstddef>
#include <cstdint>
#include <errno.h>
#include <format>
#include <string>
#include <string_view>
#include <unistd.h>

#define CT_NOINSTR __attribute__((no_instrument_function))

enum class CTColor
{
    Reset,

    Dim,
    Bold,
    Underline,
    Italic,
    Blink,
    Reverse,
    Hidden,
    Strike,

    Black,
    Red,
    Green,
    Yellow,
    Blue,
    Magenta,
    Cyan,
    White,

    Gray,
    BrightRed,
    BrightGreen,
    BrightYellow,
    BrightBlue,
    BrightMagenta,
    BrightCyan,
    BrightWhite,

    BgBlack,
    BgRed,
    BgGreen,
    BgYellow,
    BgBlue,
    BgMagenta,
    BgCyan,
    BgWhite,

    BgGray,
    BgBrightRed,
    BgBrightGreen,
    BgBrightYellow,
    BgBrightBlue,
    BgBrightMagenta,
    BgBrightCyan,
    BgBrightWhite,
};

enum class CTLevel
{
    Info    = 0,
    Warn    = 1,
    Error   = 2
};

enum {
    CT_ENTRY_EMPTY = 0,
    CT_ENTRY_USED  = 1,
    CT_ENTRY_TOMB  = 2,
    CT_ENTRY_FREED = 3
};

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
extern thread_local const char *ct_current_site;

CT_NOINSTR size_t ct_strlen(const char *str);
CT_NOINSTR int ct_streq(const char *lhs, const char *rhs);
CT_NOINSTR std::string_view ct_color(CTColor color);
CT_NOINSTR std::string_view ct_level_label(CTLevel level);
CT_NOINSTR std::string_view ct_level_color(CTLevel level);
CT_NOINSTR int ct_pid(void);
CT_NOINSTR unsigned long long ct_thread_id(void);
CT_NOINSTR const char *ct_site_name(const char *site);
CT_NOINSTR int ct_log_is_enabled(void);
CT_NOINSTR void ct_enable_logging(void);
CT_NOINSTR void ct_disable_logging(void);
CT_NOINSTR void ct_write_prefix(CTLevel level);
CT_NOINSTR void ct_write_raw(const char *data, size_t size);
CT_NOINSTR void ct_write_str(std::string_view str);
CT_NOINSTR void ct_write_cstr(const char *str);
CT_NOINSTR void ct_write_dec(size_t value);
CT_NOINSTR void ct_write_hex(uintptr_t value);
CT_NOINSTR void ct_maybe_install_backtrace(void);
CT_NOINSTR void ct_init_env_once(void);
CT_NOINSTR void ct_lock_acquire(void);
CT_NOINSTR void ct_lock_release(void);
CT_NOINSTR int ct_table_insert(void *ptr, size_t req_size, size_t size, const char *site);
CT_NOINSTR int ct_table_remove(void *ptr,
                               size_t *size_out,
                               size_t *req_size_out,
                               const char **site_out);
CT_NOINSTR int ct_table_lookup(const void *ptr,
                               size_t *size_out,
                               size_t *req_size_out,
                               const char **site_out,
                               unsigned char *state_out);
CT_NOINSTR int ct_table_lookup_containing(const void *ptr,
                                          void **base_out,
                                          size_t *size_out,
                                          size_t *req_size_out,
                                          const char **site_out,
                                          unsigned char *state_out);
CT_NOINSTR void ct_shadow_poison_range(const void *addr, size_t size);
CT_NOINSTR void ct_shadow_unpoison_range(const void *addr, size_t size);
CT_NOINSTR int ct_shadow_check_access(const void *ptr,
                                      size_t access_size,
                                      const void *base,
                                      size_t req_size,
                                      size_t alloc_size,
                                      const char *alloc_site,
                                      const char *site,
                                      int is_write,
                                      unsigned char state);
CT_NOINSTR void ct_report_bounds_error(const void *base,
                                       const void *ptr,
                                       size_t access_size,
                                       const char *site,
                                       int is_write,
                                       size_t req_size,
                                       size_t alloc_size,
                                       const char *alloc_site,
                                       unsigned char state);

template <typename... Args>
CT_NOINSTR inline void ct_log(CTLevel level, std::string_view fmt, Args&&... args)
{
    if (!ct_log_is_enabled()) {
        return;
    }
    try {
        std::string msg = std::vformat(fmt, std::make_format_args(args...));
        if (msg.empty()) {
            return;
        }

        std::string prefix;
        prefix.reserve(64);
        prefix.append(ct_color(CTColor::Dim));
        prefix.append("|");
        prefix.append(std::to_string(ct_pid()));
        prefix.append("|");
        prefix.append(ct_color(CTColor::Reset));
        prefix.push_back(' ');

        prefix.append(ct_color(CTColor::Gray));
        prefix.append(ct_color(CTColor::Italic));
        prefix.append("==ct== ");
        prefix.append(ct_color(CTColor::Reset));

        prefix.append(ct_level_color(level));
        prefix.push_back('[');
        prefix.append(ct_level_label(level));
        prefix.push_back(']');
        prefix.append(ct_color(CTColor::Reset));
        prefix.push_back(' ');

        ct_write_raw(prefix.data(), prefix.size());
        ct_write_raw(msg.data(), msg.size());
    } catch (...) {
        static const char fallback[] = "ct: log format error\n";
        ct_write_raw(fallback, ct_strlen(fallback));
    }
}

#endif // CT_RUNTIME_INTERNAL_H
