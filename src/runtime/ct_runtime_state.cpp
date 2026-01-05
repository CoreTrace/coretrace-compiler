#include "ct_runtime_internal.h"

int ct_disable_trace = 0;
int ct_disable_alloc = 0;
int ct_disable_bounds = 0;
int ct_bounds_abort = 1;
int ct_shadow_enabled = 0;
int ct_shadow_aggressive = 0;
int ct_autofree_enabled = 1;
int ct_alloc_trace_enabled = 1;
int ct_early_trace = 0;
size_t ct_early_trace_count = 0;
size_t ct_early_trace_limit = 200;
thread_local const char *ct_current_site = nullptr;
