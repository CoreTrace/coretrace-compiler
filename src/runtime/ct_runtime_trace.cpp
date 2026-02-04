#include "ct_runtime_internal.h"

#include "ct_runtime_helpers.h"

#include <format>

extern "C"
{

    CT_NOINSTR void __ct_trace_enter(const char* func)
    {
        if (!func)
        {
            return;
        }

        ct_current_site = func;
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_TRACE))
        {
            return;
        }

        if (ct_early_trace_should_log())
        {
            ct_write_prefix(CTLevel::Info);
            ct_write_str(ct_color(CTColor::Dim));
            ct_write_cstr("ct: enter ");
            ct_write_str(ct_color(CTColor::Reset));
            ct_write_cstr(func);
            ct_write_cstr("\n");
        }

        if (!ct_log_is_enabled())
        {
            if (!ct_streq(func, "main"))
            {
                return;
            }
            ct_enable_logging();
            ct_maybe_install_backtrace();
        }

        std::string demangled;
        if (ct_demangle(func, demangled))
        {
            ct_log(CTLevel::Info, "[ENTRY-FUNCTION]: -> {}{}, {}{}\n", ct_color(CTColor::Bold),
                   func, demangled, ct_color(CTColor::Reset));
        }
        else
        {
            ct_log(CTLevel::Info, "[ENTRY-FUNCTION]: -> {}{}{}\n", ct_color(CTColor::Bold), func,
                   ct_color(CTColor::Reset));
        }
    }

    CT_NOINSTR static void ct_log_exit_value(const char* func, std::string_view ret_value)
    {
        if (!func)
        {
            return;
        }

        ct_current_site = func;
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_TRACE))
        {
            return;
        }
        if (!ct_log_is_enabled())
        {
            return;
        }

        std::string demangled;
        if (ct_demangle(func, demangled))
        {
            ct_log(CTLevel::Info, "[EXIT-FUNCTION]: <- {}{}, {}{} ret={}\n",
                   ct_color(CTColor::Bold), func, demangled, ct_color(CTColor::Reset), ret_value);
        }
        else
        {
            ct_log(CTLevel::Info, "[EXIT-FUNCTION]: <- {}{}{} ret={}\n", ct_color(CTColor::Bold),
                   func, ct_color(CTColor::Reset), ret_value);
        }
    }

    CT_NOINSTR void __ct_trace_exit_void(const char* func)
    {
        ct_log_exit_value(func, "void");
    }

    CT_NOINSTR void __ct_trace_exit_i64(const char* func, long long value)
    {
        ct_log_exit_value(func, std::format("{}", value));
    }

    CT_NOINSTR void __ct_trace_exit_ptr(const char* func, const void* value)
    {
        if (!value)
        {
            ct_log_exit_value(func, "nullptr");
            return;
        }
        ct_log_exit_value(func, std::format("{:p}", value));
    }

    CT_NOINSTR void __ct_trace_exit_f64(const char* func, double value)
    {
        ct_log_exit_value(func, std::format("{}", value));
    }

    CT_NOINSTR void __ct_trace_exit_unknown(const char* func)
    {
        ct_log_exit_value(func, "<non-scalar>");
    }

} // extern "C"
