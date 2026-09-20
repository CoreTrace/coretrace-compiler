// SPDX-License-Identifier: Apache-2.0
//
// POSIX side of the runtime configuration: reads the compile-time globals, whose
// weak symbols are absent when the program was built without instrumentation, and
// runs the shared configuration sequence at start-up.
#include "ct_runtime_config.h"

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
#define CT_DECLARE_WEAK_CONFIG_GLOBAL(name) extern int name CT_WEAK_IMPORT;
    CT_RUNTIME_CONFIG_GLOBALS(CT_DECLARE_WEAK_CONFIG_GLOBAL)
#undef CT_DECLARE_WEAK_CONFIG_GLOBAL
}

namespace
{
    int ct_env_initialized = 0;
} // namespace

CT_NODISCARD CT_NOINSTR CtCompiledConfig ct_read_compiled_config(void)
{
    // An absent weak symbol has address zero; that is the uninstrumented default.
    const auto read = [](const int* value) { return value ? *value : 0; };

    CtCompiledConfig config;
#define CT_READ_WEAK_CONFIG_GLOBAL(name) config.name = read(&name);
    CT_RUNTIME_CONFIG_GLOBALS(CT_READ_WEAK_CONFIG_GLOBAL)
#undef CT_READ_WEAK_CONFIG_GLOBAL
    return config;
}

CT_NOINSTR __attribute__((constructor)) static void ct_runtime_init(void)
{
    // Diagnostics (bounds, alloc tracing, vtable) must be visible regardless of which
    // instrumentation module is linked, so logging is switched on here rather than by
    // the first module that happens to run.
    ct_enable_logging();
    ct_maybe_install_backtrace();
    ct_apply_runtime_config();
}

CT_NOINSTR void ct_init_env_once(void)
{
    int expected = 0;
    if (!__atomic_compare_exchange_n(&ct_env_initialized, &expected, 1, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
    {
        return;
    }

    ct_enable_logging();
    ct_apply_runtime_config();
}
