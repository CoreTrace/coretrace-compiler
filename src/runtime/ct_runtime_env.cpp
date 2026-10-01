// SPDX-License-Identifier: Apache-2.0
//
// POSIX side of the runtime configuration: reads the compile-time globals, whose
// weak symbols are absent when the program was built without instrumentation, and
// runs the shared configuration sequence at start-up.
#include "ct_runtime_config.h"
#include "ct_runtime_internal.h"

#include <pthread.h>

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

// fork() copies only the calling thread: a lock another thread held at that moment would
// stay held in the child, which would hang on its first allocation or log line. Around
// fork(), the calling thread takes every lock of the runtime, in the order the runtime
// nests them, and releases them in both processes.
CT_NOINSTR static void ct_lock_runtime_for_fork(void)
{
    ct_lock_acquire();
    ct_shadow_lock_acquire();
    ct_log_lock_acquire();
}

CT_NOINSTR static void ct_unlock_runtime_after_fork(void)
{
    ct_log_lock_release();
    ct_shadow_lock_release();
    ct_lock_release();
}

CT_NOINSTR __attribute__((constructor)) static void ct_runtime_init(void)
{
    // Diagnostics (bounds, alloc tracing, vtable) must be visible regardless of which
    // instrumentation module is linked, so logging is switched on here rather than by
    // the first module that happens to run.
    ct_enable_logging();
    ct_maybe_install_backtrace();
    ct_apply_runtime_config();
    pthread_atfork(ct_lock_runtime_for_fork, ct_unlock_runtime_after_fork,
                   ct_unlock_runtime_after_fork);
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
