// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_internal.h"

#include <cstdint>
#include <cstdlib>

CT_NOINSTR void ct_report_bounds_error(const void* base, const void* ptr, size_t access_size,
                                       const char* site, int is_write, size_t req_size,
                                       size_t alloc_size, const char* alloc_site,
                                       unsigned char state)
{
    uintptr_t base_addr = reinterpret_cast<uintptr_t>(base);
    uintptr_t ptr_addr = reinterpret_cast<uintptr_t>(ptr);
    long long signed_offset = 0;
    if (ptr_addr >= base_addr)
    {
        signed_offset = static_cast<long long>(ptr_addr - base_addr);
    }
    else
    {
        signed_offset = -static_cast<long long>(base_addr - ptr_addr);
    }

    const char* kind = (state == CT_ENTRY_FREED) ? "heap-use-after-free" : "heap-buffer-overflow";
    size_t report_size = req_size ? req_size : alloc_size;

    ct_log(CTLevel::Error,
           "ct: {} {} of size {}\n"
           "  access={} ptr={:p} offset={}\n"
           "  alloc_size={} alloc_site={} base={:p}\n",
           kind, is_write ? "WRITE" : "READ", access_size, ct_site_name(site), ptr, signed_offset,
           report_size, ct_site_name(alloc_site), base);

    if (alloc_size != report_size)
    {
        ct_log(CTLevel::Error, "  usable_size={}\n", alloc_size);
    }

    if (ct_bounds_abort_enabled())
    {
        abort();
    }
}

// True when [ptr, ptr + access_size) is not inside [base, base + bound_size).
CT_NODISCARD CT_NOINSTR static bool ct_outside_allocation(const void* ptr, size_t access_size,
                                                          const void* base, size_t bound_size)
{
    uintptr_t base_addr = reinterpret_cast<uintptr_t>(base);
    uintptr_t ptr_addr = reinterpret_cast<uintptr_t>(ptr);
    if (ptr_addr < base_addr)
    {
        return true;
    }
    size_t offset = static_cast<size_t>(ptr_addr - base_addr);
    return offset > bound_size || access_size > (bound_size - offset);
}

extern "C"
{

    CT_NOINSTR void __ct_check_bounds(const void* base, const void* ptr, size_t access_size,
                                      const char* site, int is_write)
    {
        if (!ct_is_enabled(CT_FEATURE_BOUNDS))
        {
            return;
        }
        ct_init_env_once();
        if (!ptr || access_size == 0)
        {
            return;
        }

        size_t alloc_size = 0;
        size_t req_size = 0;
        const char* alloc_site = nullptr;
        unsigned char state = 0;
        int found = 0;
        const void* alloc_base = base;

        if (!base)
        {
            return;
        }

        ct_lock_acquire();
        found = ct_table_lookup(base, &alloc_size, &req_size, &alloc_site, &state);
        const bool base_is_allocation = found != 0;
        if (!found && ct_is_enabled(CT_FEATURE_SHADOW) && ct_is_enabled(CT_FEATURE_SHADOW_AGGR))
        {
            void* found_base = nullptr;
            found = ct_table_lookup_containing(ptr, &found_base, &alloc_size, &req_size,
                                               &alloc_site, &state);
            if (found && found_base)
            {
                alloc_base = found_base;
            }
        }
        ct_lock_release();

        if (!found)
        {
            return;
        }

        if (state == CT_ENTRY_FREED && !ct_is_enabled(CT_FEATURE_SHADOW))
        {
            ct_report_bounds_error(alloc_base, ptr, access_size, site, is_write, req_size,
                                   alloc_size, alloc_site, state);
            return;
        }

        const bool oob =
            ct_outside_allocation(ptr, access_size, alloc_base, req_size ? req_size : alloc_size);

        if (ct_is_enabled(CT_FEATURE_SHADOW))
        {
            // Shadow bytes say which bytes are valid, not which object they belong to, so
            // an access into a neighbouring allocation's live bytes looks valid to them.
            // When the base itself named the allocation, its bounds decide first.
            if (base_is_allocation && oob)
            {
                ct_report_bounds_error(alloc_base, ptr, access_size, site, is_write, req_size,
                                       alloc_size, alloc_site, state);
                return;
            }
            (void)ct_shadow_check_access(ptr, access_size, alloc_base, req_size, alloc_size,
                                         alloc_site, site, is_write, state);
            return;
        }

        if (!oob)
        {
            return;
        }

        ct_report_bounds_error(alloc_base, ptr, access_size, site, is_write, req_size, alloc_size,
                               alloc_site, state);
    }

} // extern "C"
