// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_internal.h"

#include <cstdint>
#include <cstdlib>

// Reports an access outside the object at `base`, of `object_size` bytes of which
// `usable_size` are allocated, created at `object_site`.
CT_NOINSTR static void ct_report_out_of_bounds(const char* kind, const void* base, const void* ptr,
                                               size_t access_size, const char* site, int is_write,
                                               size_t object_size, size_t usable_size,
                                               const char* object_site)
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

    ct_log(CTLevel::Error,
           "ct: {} {} of size {}\n"
           "  access={} ptr={:p} offset={}\n"
           "  alloc_size={} alloc_site={} base={:p}\n",
           kind, is_write ? "WRITE" : "READ", access_size, ct_site_name(site), ptr, signed_offset,
           object_size, ct_site_name(object_site), base);

    if (usable_size != object_size)
    {
        ct_log(CTLevel::Error, "  usable_size={}\n", usable_size);
    }

    if (ct_bounds_abort_enabled())
    {
        abort();
    }
}

CT_NOINSTR void ct_report_bounds_error(const void* base, const void* ptr, size_t access_size,
                                       const char* site, int is_write, size_t req_size,
                                       size_t alloc_size, const char* alloc_site,
                                       unsigned char state)
{
    const char* kind = (state == CT_ENTRY_FREED) ? "heap-use-after-free" : "heap-buffer-overflow";
    ct_report_out_of_bounds(kind, base, ptr, access_size, site, is_write,
                            req_size ? req_size : alloc_size, alloc_size, alloc_site);
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

namespace
{
    struct ct_stack_object
    {
        const void* base;
        size_t size;
        const char* site;
    };

    // The stack objects this thread's running frames registered, innermost last. The
    // array is fixed so that registering never allocates; objects beyond it are not
    // registered, and their accesses not checked. A frame that an exception or a longjmp
    // leaves without returning keeps its objects here until an outer frame returns;
    // lookups take the newest match, so an object registered later at the same address
    // wins.
    constexpr size_t kCtStackObjectCapacity = 512;
    thread_local ct_stack_object ct_stack_objects[kCtStackObjectCapacity];
    thread_local size_t ct_stack_depth = 0;

    // Newest registered object starting at `base` or, when `containing`, holding it.
    CT_NODISCARD CT_NOINSTR const ct_stack_object* ct_stack_find(const void* base, bool containing)
    {
        const uintptr_t address = reinterpret_cast<uintptr_t>(base);
        for (size_t i = ct_stack_depth; i-- > 0;)
        {
            const ct_stack_object& object = ct_stack_objects[i];
            const uintptr_t start = reinterpret_cast<uintptr_t>(object.base);
            if (address == start ||
                (containing && address > start && address - start < object.size))
            {
                return &object;
            }
        }
        return nullptr;
    }
} // namespace

extern "C"
{

    CT_NOINSTR size_t __ct_stack_push(const void* base, size_t size, const char* site)
    {
        const size_t depth = ct_stack_depth;
        if (depth < kCtStackObjectCapacity)
        {
            ct_stack_objects[depth] = {base, size, site};
            ct_stack_depth = depth + 1;
        }
        return depth;
    }

    CT_NOINSTR void __ct_stack_pop(size_t depth)
    {
        if (depth < ct_stack_depth)
        {
            ct_stack_depth = depth;
        }
    }

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
            // Not a heap block; perhaps an object of a running frame. Shadow memory does not
            // describe the stack, so the object's bounds alone decide.
            const bool aggressive =
                ct_is_enabled(CT_FEATURE_SHADOW) && ct_is_enabled(CT_FEATURE_SHADOW_AGGR);
            const ct_stack_object* object = ct_stack_find(base, aggressive);
            if (object && ct_outside_allocation(ptr, access_size, object->base, object->size))
            {
                ct_report_out_of_bounds("stack-buffer-overflow", object->base, ptr, access_size,
                                        site, is_write, object->size, object->size, object->site);
            }
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
