// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_internal.h"

#include "ct_runtime_helpers.h"

#include <atomic>
#include <format>

namespace
{
    // Function entry/exit tracing stays silent until main is entered so static
    // initialisers do not flood the output. This is a trace-module policy only; the
    // logger itself is enabled at runtime initialisation.
    std::atomic<int> ct_trace_started{0};

    CT_NODISCARD CT_NOINSTR int ct_trace_is_started(void)
    {
        return ct_trace_started.load(std::memory_order_acquire);
    }
} // namespace

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

        if (!ct_trace_is_started())
        {
            if (!ct_streq(func, "main"))
            {
                return;
            }
            ct_trace_started.store(1, std::memory_order_release);
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
        if (!ct_trace_is_started())
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
