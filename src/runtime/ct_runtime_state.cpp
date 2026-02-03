#include "ct_runtime_internal.h"

#include <atomic>

namespace
{
    constexpr uint64_t kDefaultFeatures = CT_FEATURE_TRACE | CT_FEATURE_ALLOC | CT_FEATURE_BOUNDS |
                                          CT_FEATURE_AUTOFREE | CT_FEATURE_ALLOC_TRACE;

    std::atomic<uint64_t> ct_feature_flags{kDefaultFeatures};
    std::atomic<int> ct_bounds_abort_state{1};
    std::atomic<size_t> ct_early_trace_count_state{0};
    std::atomic<size_t> ct_early_trace_limit_state{200};

    CT_NOINSTR void ct_sync_legacy_flags(uint64_t features)
    {
        ct_disable_trace = (features & CT_FEATURE_TRACE) ? 0 : 1;
        ct_disable_alloc = (features & CT_FEATURE_ALLOC) ? 0 : 1;
        ct_disable_bounds = (features & CT_FEATURE_BOUNDS) ? 0 : 1;
        ct_shadow_enabled = (features & CT_FEATURE_SHADOW) ? 1 : 0;
        ct_shadow_aggressive = (features & CT_FEATURE_SHADOW_AGGR) ? 1 : 0;
        ct_autofree_enabled = (features & CT_FEATURE_AUTOFREE) ? 1 : 0;
        ct_alloc_trace_enabled = (features & CT_FEATURE_ALLOC_TRACE) ? 1 : 0;
        ct_vtable_diag_enabled = (features & CT_FEATURE_VTABLE_DIAG) ? 1 : 0;
        ct_early_trace = (features & CT_FEATURE_EARLY_TRACE) ? 1 : 0;

        ct_bounds_abort = ct_bounds_abort_state.load(std::memory_order_relaxed);
        ct_early_trace_limit = ct_early_trace_limit_state.load(std::memory_order_relaxed);
        ct_early_trace_count = ct_early_trace_count_state.load(std::memory_order_relaxed);
    }
} // namespace

int ct_disable_trace = 0;
int ct_disable_alloc = 0;
int ct_disable_bounds = 0;
int ct_bounds_abort = 1;
int ct_shadow_enabled = 0;
int ct_shadow_aggressive = 0;
int ct_autofree_enabled = 1;
int ct_alloc_trace_enabled = 1;
int ct_vtable_diag_enabled = 0;
int ct_alloc_disabled_by_config = 0;
int ct_alloc_disabled_by_env = 0;
int ct_early_trace = 0;
size_t ct_early_trace_count = 0;
size_t ct_early_trace_limit = 200;
thread_local const char* ct_current_site = nullptr;

namespace
{
    struct CtRuntimeLegacyInit
    {
        CT_NOINSTR CtRuntimeLegacyInit()
        {
            ct_sync_legacy_flags(ct_feature_flags.load(std::memory_order_relaxed));
        }
    };

    CtRuntimeLegacyInit ct_runtime_legacy_init;
} // namespace

extern "C"
{
    CT_NODISCARD CT_NOINSTR int ct_is_enabled(uint64_t feature)
    {
        return (ct_feature_flags.load(std::memory_order_relaxed) & feature) != 0;
    }

    CT_NOINSTR void ct_set_enabled(uint64_t feature, int enabled)
    {
        if (enabled)
        {
            uint64_t previous =
                ct_feature_flags.fetch_or(feature, std::memory_order_relaxed);
            ct_sync_legacy_flags(previous | feature);
            return;
        }

        uint64_t previous =
            ct_feature_flags.fetch_and(~feature, std::memory_order_relaxed);
        ct_sync_legacy_flags(previous & ~feature);
    }

    CT_NODISCARD CT_NOINSTR uint64_t ct_get_features(void)
    {
        return ct_feature_flags.load(std::memory_order_relaxed);
    }

    CT_NODISCARD CT_NOINSTR int ct_bounds_abort_enabled(void)
    {
        return ct_bounds_abort_state.load(std::memory_order_relaxed) != 0;
    }

    CT_NOINSTR void ct_set_bounds_abort(int enabled)
    {
        ct_bounds_abort_state.store(enabled ? 1 : 0, std::memory_order_relaxed);
        ct_bounds_abort = enabled ? 1 : 0;
    }

    CT_NODISCARD CT_NOINSTR int ct_early_trace_should_log(void)
    {
        if (!ct_is_enabled(CT_FEATURE_EARLY_TRACE))
        {
            return 0;
        }

        const size_t limit = ct_early_trace_limit_state.load(std::memory_order_relaxed);
        size_t current = ct_early_trace_count_state.load(std::memory_order_relaxed);
        while (current < limit)
        {
            if (ct_early_trace_count_state.compare_exchange_weak(
                    current, current + 1, std::memory_order_relaxed,
                    std::memory_order_relaxed))
            {
                ct_early_trace_count = current + 1;
                return 1;
            }
        }
        return 0;
    }
}
