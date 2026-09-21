// SPDX-License-Identifier: Apache-2.0
//
// The part of the vtable diagnostics that does not depend on the C++ ABI. Everything
// else, reading the vptr and its type information and enumerating loaded modules, is
// genuinely different between the Itanium and MSVC ABIs and stays in the per-platform
// files.
#include "ct_runtime_vtable_shared.h"

CT_NODISCARD CT_NOINSTR bool ct_is_unknown_type(const char* type_name)
{
    if (!type_name || type_name[0] == '\0')
    {
        return true;
    }
    return ct_streq(type_name, "<unknown>") != 0;
}

CT_NODISCARD CT_NOINSTR bool ct_object_is_freed(const void* this_ptr)
{
    if (!this_ptr || !ct_is_enabled(CT_FEATURE_ALLOC))
    {
        return false;
    }
    unsigned char state = 0;
    ct_lock_acquire();
    const int found =
        ct_table_lookup_containing(this_ptr, nullptr, nullptr, nullptr, nullptr, &state);
    ct_lock_release();
    return found && state == CT_ENTRY_FREED;
}
