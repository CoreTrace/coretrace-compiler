// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_config.h"

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
} // namespace

CT_NODISCARD CT_NOINSTR CtCompiledConfig ct_read_compiled_config(void)
{
    // /alternatename above guarantees every global resolves, so no null check.
    CtCompiledConfig config;
#define CT_READ_CONFIG_GLOBAL(name) config.name = name;
    CT_RUNTIME_CONFIG_GLOBALS(CT_READ_CONFIG_GLOBAL)
#undef CT_READ_CONFIG_GLOBAL
    return config;
}

namespace
{
    struct CtRuntimeInit
    {
        CT_NOINSTR CtRuntimeInit()
        {
            // See the POSIX runtime: logging is enabled once at initialisation so
            // diagnostics do not depend on which module runs first.
            ct_enable_logging();
            ct_maybe_install_backtrace();
            ct_apply_runtime_config();
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

    ct_enable_logging();
    ct_apply_runtime_config();
}
