// SPDX-License-Identifier: Apache-2.0
//
// Internals of the POSIX allocation tracker, shared between the table and ABI
// translation unit (ct_runtime_alloc.cpp), the conservative auto-free scan
// (ct_runtime_autofree_scan.cpp) and the Objective-C object tracking
// (ct_runtime_objc.cpp). Not part of any public interface.
//
// Every declaration whose comment says "ct_alloc_lock held" must be reached with
// the allocator spinlock taken; the scan takes it around its table walks.
#ifndef CT_RUNTIME_ALLOC_INTERNAL_H
#define CT_RUNTIME_ALLOC_INTERNAL_H

#include "ct_runtime_internal.h"

#include <cstddef>
#include <cstdint>

struct ct_alloc_entry
{
    void* ptr;
    size_t size;
    size_t req_size;
    const char* site;
    unsigned char state;
    unsigned char kind;
    unsigned char mark;
};

enum
{
    CT_ALLOC_KIND_MALLOC = 0,
    CT_ALLOC_KIND_NEW = 1,
    CT_ALLOC_KIND_NEW_ARRAY = 2,
    CT_ALLOC_KIND_MMAP = 3,
    CT_ALLOC_KIND_SBRK = 4,
    // An Objective-C object: the Objective-C runtime owns its memory and releases it.
    CT_ALLOC_KIND_OBJC = 5
};

// One allocation the scan proved unreachable and is about to release.
struct ct_autofree_free_item
{
    void* ptr;
    size_t size;
    const char* site;
    unsigned char kind;
};

// The open-addressed table. The storage moves when it grows, so the scan must
// re-read `ct_alloc_table` under the lock rather than cache it.
extern struct ct_alloc_entry* ct_alloc_table;
extern size_t ct_alloc_table_size;
extern size_t ct_alloc_table_mask;
extern size_t ct_alloc_count;

CT_NODISCARD CT_NOINSTR size_t ct_hash_ptr(const void* ptr, size_t mask);

// Entry holding exactly `ptr`, in any state; nullptr when absent.
// ct_alloc_lock held.
CT_NODISCARD CT_NOINSTR struct ct_alloc_entry* ct_table_find_entry(const void* ptr);

// Blocks another allocator hands out and takes back, such as Objective-C objects,
// tracked like the tracker's own blocks while they live. ct_forget_allocation drops the
// live entry of `ptr` if it has `kind` and returns its size, 0 when there is none. It
// must run before the owner releases the memory, and keeps no freed record: the owner
// reuses that memory without telling the runtime.
CT_NOINSTR void ct_track_allocation(void* ptr, size_t size, const char* site, unsigned char kind);
CT_NODISCARD CT_NOINSTR size_t ct_forget_allocation(void* ptr, unsigned char kind);

// Conservative auto-free scan. A no-op build is provided where the platform has
// no thread-suspension support, so callers never need a guard.
CT_NOINSTR void ct_autofree_scan_init_once(void);

// True when the scan is enabled and configured to vet individual pointers, so
// __ct_autofree must offer it a pointer before releasing it.
CT_NODISCARD CT_NOINSTR int ct_autofree_scan_checks_pointers(void);

// Non-zero when the scan found `ptr` still reachable and it must not be released.
CT_NODISCARD CT_NOINSTR int ct_autofree_scan_for_ptr(void* ptr, size_t size);

#endif // CT_RUNTIME_ALLOC_INTERNAL_H
