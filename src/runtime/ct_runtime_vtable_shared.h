// SPDX-License-Identifier: Apache-2.0
//
// ABI-independent helpers of the vtable diagnostics, shared by both platform ports.
#ifndef CT_RUNTIME_VTABLE_SHARED_H
#define CT_RUNTIME_VTABLE_SHARED_H

#include "ct_runtime_internal.h"

// True for a type name the diagnostics could not determine.
CT_NODISCARD CT_NOINSTR bool ct_is_unknown_type(const char* type_name);

// True when alloc tracking knows this object was already released. Checked before
// any read through the pointer: a freed object's vptr is garbage and following it
// would crash the diagnostic that is supposed to report the use-after-free.
CT_NODISCARD CT_NOINSTR bool ct_object_is_freed(const void* this_ptr);

#endif // CT_RUNTIME_VTABLE_SHARED_H
