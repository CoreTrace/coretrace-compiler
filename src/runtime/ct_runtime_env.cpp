#include "ct_runtime_internal.h"

#include <cstdlib>

namespace
{
#if defined(__APPLE__)
#define CT_WEAK_IMPORT __attribute__((weak_import))
#else
#define CT_WEAK_IMPORT __attribute__((weak))
#endif
} // namespace

extern "C"
{
    extern int __ct_config_shadow CT_WEAK_IMPORT;
    extern int __ct_config_shadow_aggressive CT_WEAK_IMPORT;
    extern int __ct_config_bounds_no_abort CT_WEAK_IMPORT;
    extern int __ct_config_disable_alloc CT_WEAK_IMPORT;
    extern int __ct_config_disable_autofree CT_WEAK_IMPORT;
    extern int __ct_config_disable_alloc_trace CT_WEAK_IMPORT;
    extern int __ct_config_vtable_diag CT_WEAK_IMPORT;
}

namespace
{
    int ct_env_initialized = 0;
}

CT_NOINSTR static void ct_apply_compiled_config(void)
{
    auto readWeak = [](const int* ptr) -> int { return ptr ? *ptr : 0; };

    if (readWeak(&__ct_config_shadow) || readWeak(&__ct_config_shadow_aggressive))
    {
        ct_set_enabled(CT_FEATURE_SHADOW, 1);
    }
    if (readWeak(&__ct_config_shadow_aggressive))
    {
        ct_set_enabled(CT_FEATURE_SHADOW_AGGR, 1);
    }
    if (readWeak(&__ct_config_bounds_no_abort))
    {
        ct_set_bounds_abort(0);
    }
    if (readWeak(&__ct_config_disable_alloc))
    {
        ct_set_enabled(CT_FEATURE_ALLOC, 0);
        ct_alloc_disabled_by_config = 1;
    }
    if (readWeak(&__ct_config_disable_autofree))
    {
        ct_set_enabled(CT_FEATURE_AUTOFREE, 0);
    }
    if (readWeak(&__ct_config_disable_alloc_trace))
    {
        ct_set_enabled(CT_FEATURE_ALLOC_TRACE, 0);
    }
    if (readWeak(&__ct_config_vtable_diag))
    {
        ct_set_enabled(CT_FEATURE_VTABLE_DIAG, 1);
    }
}

CT_NOINSTR __attribute__((constructor)) static void ct_runtime_init(void)
{
    ct_maybe_install_backtrace();
    ct_apply_compiled_config();
    if (getenv("CT_DISABLE_TRACE") != nullptr)
    {
        ct_set_enabled(CT_FEATURE_TRACE, 0);
    }
    if (getenv("CT_DISABLE_ALLOC") != nullptr)
    {
        ct_set_enabled(CT_FEATURE_ALLOC, 0);
        ct_alloc_disabled_by_env = 1;
    }
    if (getenv("CT_EARLY_TRACE") != nullptr)
    {
        ct_set_enabled(CT_FEATURE_EARLY_TRACE, 1);
    }
    if (getenv("CT_DISABLE_BOUNDS") != nullptr)
    {
        ct_set_enabled(CT_FEATURE_BOUNDS, 0);
    }
    if (getenv("CT_BOUNDS_NO_ABORT") != nullptr)
    {
        ct_set_bounds_abort(0);
    }
    if (getenv("CT_SHADOW") != nullptr)
    {
        ct_set_enabled(CT_FEATURE_SHADOW, 1);
    }
    if (getenv("CT_SHADOW_AGGRESSIVE") != nullptr)
    {
        ct_set_enabled(CT_FEATURE_SHADOW, 1);
        ct_set_enabled(CT_FEATURE_SHADOW_AGGR, 1);
    }
    if (getenv("CT_DISABLE_AUTOFREE") != nullptr)
    {
        ct_set_enabled(CT_FEATURE_AUTOFREE, 0);
    }
    if (getenv("CT_DISABLE_ALLOC_TRACE") != nullptr)
    {
        ct_set_enabled(CT_FEATURE_ALLOC_TRACE, 0);
    }
}

CT_NOINSTR void ct_init_env_once(void)
{
    int expected = 0;
    if (!__atomic_compare_exchange_n(&ct_env_initialized, &expected, 1, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
    {
        return;
    }

    ct_apply_compiled_config();
    if (getenv("CT_DISABLE_TRACE") != nullptr)
    {
        ct_set_enabled(CT_FEATURE_TRACE, 0);
    }
    if (getenv("CT_DISABLE_ALLOC") != nullptr)
    {
        ct_set_enabled(CT_FEATURE_ALLOC, 0);
        ct_alloc_disabled_by_env = 1;
    }
    if (getenv("CT_EARLY_TRACE") != nullptr)
    {
        ct_set_enabled(CT_FEATURE_EARLY_TRACE, 1);
    }
    if (getenv("CT_DISABLE_BOUNDS") != nullptr)
    {
        ct_set_enabled(CT_FEATURE_BOUNDS, 0);
    }
    if (getenv("CT_BOUNDS_NO_ABORT") != nullptr)
    {
        ct_set_bounds_abort(0);
    }
    if (getenv("CT_SHADOW") != nullptr)
    {
        ct_set_enabled(CT_FEATURE_SHADOW, 1);
    }
    if (getenv("CT_SHADOW_AGGRESSIVE") != nullptr)
    {
        ct_set_enabled(CT_FEATURE_SHADOW, 1);
        ct_set_enabled(CT_FEATURE_SHADOW_AGGR, 1);
    }
    if (getenv("CT_DISABLE_AUTOFREE") != nullptr)
    {
        ct_set_enabled(CT_FEATURE_AUTOFREE, 0);
    }
    if (getenv("CT_DISABLE_ALLOC_TRACE") != nullptr)
    {
        ct_set_enabled(CT_FEATURE_ALLOC_TRACE, 0);
    }
}
