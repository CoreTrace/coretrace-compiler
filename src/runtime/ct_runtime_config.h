// SPDX-License-Identifier: Apache-2.0
//
// Runtime configuration, shared by both platform ports. The meaning of the settings
// lives here; each platform only supplies the values of the compile-time globals,
// because how an absent weak symbol reads is platform-specific.
#ifndef CT_RUNTIME_CONFIG_H
#define CT_RUNTIME_CONFIG_H

#include "ct_runtime_internal.h"

// Values of the __ct_config_* globals the compiler emits into instrumented modules,
// one field per global, named after it without the prefix. Zero everywhere means a
// program built without instrumentation options.
struct CtCompiledConfig
{
#define CT_DECLARE_COMPILED_CONFIG_FIELD(name) int name = 0;
    CT_RUNTIME_CONFIG_GLOBALS(CT_DECLARE_COMPILED_CONFIG_FIELD)
#undef CT_DECLARE_COMPILED_CONFIG_FIELD
};

// Applies the options the program was built with. Called before the environment so
// CT_* variables override them.
CT_NOINSTR void ct_apply_compiled_config(const CtCompiledConfig& config);

// Applies the CT_* environment variables. Platform-neutral.
CT_NOINSTR void ct_apply_env_config(void);

// Reads the weak __ct_config_* globals of this platform.
CT_NODISCARD CT_NOINSTR CtCompiledConfig ct_read_compiled_config(void);

// Compiled options then environment, in that order. Runs the full sequence on every
// call; it is idempotent, and ct_init_env_once guards the lazy path.
CT_NOINSTR void ct_apply_runtime_config(void);

#endif // CT_RUNTIME_CONFIG_H
