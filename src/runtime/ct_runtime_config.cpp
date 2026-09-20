// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_config.h"

#include <cstdlib>

CT_NOINSTR void ct_apply_compiled_config(const CtCompiledConfig& config)
{
    if (config.__ct_config_shadow || config.__ct_config_shadow_aggressive)
    {
        ct_set_enabled(CT_FEATURE_SHADOW, 1);
    }
    if (config.__ct_config_shadow_aggressive)
    {
        ct_set_enabled(CT_FEATURE_SHADOW_AGGR, 1);
    }
    if (config.__ct_config_bounds_no_abort)
    {
        ct_set_bounds_abort(0);
    }
    if (config.__ct_config_disable_alloc)
    {
        ct_set_enabled(CT_FEATURE_ALLOC, 0);
        ct_alloc_disabled_by_config = 1;
    }
    if (config.__ct_config_disable_autofree)
    {
        ct_set_enabled(CT_FEATURE_AUTOFREE, 0);
    }
    if (config.__ct_config_disable_alloc_trace)
    {
        ct_set_enabled(CT_FEATURE_ALLOC_TRACE, 0);
    }
    if (config.__ct_config_vtable_diag)
    {
        ct_set_enabled(CT_FEATURE_VTABLE_DIAG, 1);
    }
}

CT_NOINSTR void ct_apply_env_config(void)
{
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // MSVC warning: 'getenv': This function or variable may be unsafe
#endif
    // Presence of the variable is what counts; its value is not read.
    const auto is_set = [](const char* name) { return std::getenv(name) != nullptr; };

    if (is_set("CT_DISABLE_TRACE"))
    {
        ct_set_enabled(CT_FEATURE_TRACE, 0);
    }
    if (is_set("CT_DISABLE_ALLOC"))
    {
        ct_set_enabled(CT_FEATURE_ALLOC, 0);
        ct_alloc_disabled_by_env = 1;
    }
    if (is_set("CT_EARLY_TRACE"))
    {
        ct_set_enabled(CT_FEATURE_EARLY_TRACE, 1);
    }
    if (is_set("CT_DISABLE_BOUNDS"))
    {
        ct_set_enabled(CT_FEATURE_BOUNDS, 0);
    }
    if (is_set("CT_BOUNDS_NO_ABORT"))
    {
        ct_set_bounds_abort(0);
    }
    if (is_set("CT_SHADOW"))
    {
        ct_set_enabled(CT_FEATURE_SHADOW, 1);
    }
    if (is_set("CT_SHADOW_AGGRESSIVE"))
    {
        ct_set_enabled(CT_FEATURE_SHADOW, 1);
        ct_set_enabled(CT_FEATURE_SHADOW_AGGR, 1);
    }
    if (is_set("CT_DISABLE_AUTOFREE"))
    {
        ct_set_enabled(CT_FEATURE_AUTOFREE, 0);
    }
    if (is_set("CT_DISABLE_ALLOC_TRACE"))
    {
        ct_set_enabled(CT_FEATURE_ALLOC_TRACE, 0);
    }
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

CT_NOINSTR void ct_apply_runtime_config(void)
{
    ct_apply_compiled_config(ct_read_compiled_config());
    ct_apply_env_config();
}
