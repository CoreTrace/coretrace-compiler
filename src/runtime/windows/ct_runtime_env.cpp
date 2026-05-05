// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_internal.h"

#include <atomic>
#include <cstdlib>

#if defined(_M_IX86)
#define CT_ALTNAME(symbol, fallback)                                                               \
    __pragma(comment(linker, "/alternatename:_" #symbol "=_" #fallback))
#else
#define CT_ALTNAME(symbol, fallback)                                                               \
    __pragma(comment(linker, "/alternatename:" #symbol "=" #fallback))
#endif

extern "C"
{
    __declspec(selectany) int __ct_config_shadow_default = 0;
    __declspec(selectany) int __ct_config_shadow_aggressive_default = 0;
    __declspec(selectany) int __ct_config_bounds_no_abort_default = 0;
    __declspec(selectany) int __ct_config_disable_alloc_default = 0;
    __declspec(selectany) int __ct_config_disable_autofree_default = 0;
    __declspec(selectany) int __ct_config_disable_alloc_trace_default = 0;
    __declspec(selectany) int __ct_config_vtable_diag_default = 0;

    CT_ALTNAME(__ct_config_shadow, __ct_config_shadow_default)
    CT_ALTNAME(__ct_config_shadow_aggressive, __ct_config_shadow_aggressive_default)
    CT_ALTNAME(__ct_config_bounds_no_abort, __ct_config_bounds_no_abort_default)
    CT_ALTNAME(__ct_config_disable_alloc, __ct_config_disable_alloc_default)
    CT_ALTNAME(__ct_config_disable_autofree, __ct_config_disable_autofree_default)
    CT_ALTNAME(__ct_config_disable_alloc_trace, __ct_config_disable_alloc_trace_default)
    CT_ALTNAME(__ct_config_vtable_diag, __ct_config_vtable_diag_default)

    extern int __ct_config_shadow;
    extern int __ct_config_shadow_aggressive;
    extern int __ct_config_bounds_no_abort;
    extern int __ct_config_disable_alloc;
    extern int __ct_config_disable_autofree;
    extern int __ct_config_disable_alloc_trace;
    extern int __ct_config_vtable_diag;
}

namespace
{
    std::atomic<int> ct_env_initialized{0};

    CT_NOINSTR void ct_apply_compiled_config(void)
    {
        if (__ct_config_shadow || __ct_config_shadow_aggressive)
        {
            ct_set_enabled(CT_FEATURE_SHADOW, 1);
        }
        if (__ct_config_shadow_aggressive)
        {
            ct_set_enabled(CT_FEATURE_SHADOW_AGGR, 1);
        }
        if (__ct_config_bounds_no_abort)
        {
            ct_set_bounds_abort(0);
        }
        if (__ct_config_disable_alloc)
        {
            ct_set_enabled(CT_FEATURE_ALLOC, 0);
            ct_alloc_disabled_by_config = 1;
        }
        if (__ct_config_disable_autofree)
        {
            ct_set_enabled(CT_FEATURE_AUTOFREE, 0);
        }
        if (__ct_config_disable_alloc_trace)
        {
            ct_set_enabled(CT_FEATURE_ALLOC_TRACE, 0);
        }
        if (__ct_config_vtable_diag)
        {
            ct_set_enabled(CT_FEATURE_VTABLE_DIAG, 1);
        }
    }

    CT_NOINSTR void ct_apply_env_config(void)
    {
        if (std::getenv("CT_DISABLE_TRACE") != nullptr)
        {
            ct_set_enabled(CT_FEATURE_TRACE, 0);
        }
        if (std::getenv("CT_DISABLE_ALLOC") != nullptr)
        {
            ct_set_enabled(CT_FEATURE_ALLOC, 0);
            ct_alloc_disabled_by_env = 1;
        }
        if (std::getenv("CT_EARLY_TRACE") != nullptr)
        {
            ct_set_enabled(CT_FEATURE_EARLY_TRACE, 1);
        }
        if (std::getenv("CT_DISABLE_BOUNDS") != nullptr)
        {
            ct_set_enabled(CT_FEATURE_BOUNDS, 0);
        }
        if (std::getenv("CT_BOUNDS_NO_ABORT") != nullptr)
        {
            ct_set_bounds_abort(0);
        }
        if (std::getenv("CT_SHADOW") != nullptr)
        {
            ct_set_enabled(CT_FEATURE_SHADOW, 1);
        }
        if (std::getenv("CT_SHADOW_AGGRESSIVE") != nullptr)
        {
            ct_set_enabled(CT_FEATURE_SHADOW, 1);
            ct_set_enabled(CT_FEATURE_SHADOW_AGGR, 1);
        }
        if (std::getenv("CT_DISABLE_AUTOFREE") != nullptr)
        {
            ct_set_enabled(CT_FEATURE_AUTOFREE, 0);
        }
        if (std::getenv("CT_DISABLE_ALLOC_TRACE") != nullptr)
        {
            ct_set_enabled(CT_FEATURE_ALLOC_TRACE, 0);
        }
    }

    struct CtRuntimeInit
    {
        CT_NOINSTR CtRuntimeInit()
        {
            ct_maybe_install_backtrace();
            ct_apply_compiled_config();
            ct_apply_env_config();
        }
    };

    CtRuntimeInit ct_runtime_init;
} // namespace

CT_NOINSTR void ct_init_env_once(void)
{
    int expected = 0;
    if (!ct_env_initialized.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
    {
        return;
    }

    ct_apply_compiled_config();
    ct_apply_env_config();
}
