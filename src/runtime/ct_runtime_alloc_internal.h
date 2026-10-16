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
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

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

// The function a block is released with, in the program's call or once it leaves the
// quarantine (ct_runtime_quarantine.h), where it is stored as ct_quarantine_item::api.
enum class CtReleaseApi : unsigned char
{
    Free,
    Delete,
    DeleteArray,
    DeleteNothrow,
    DeleteArrayNothrow,
    DeleteDestroying,
    DeleteArrayDestroying
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

// Bounds of the initial storage of the table and of its index, static arrays in the image's
// data. Until they grow, they hold a pointer to every tracked block, which the scan must not
// take for references to them.
CT_NOINSTR void ct_alloc_table_storage_bounds(uintptr_t* begin, uintptr_t* end);

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

// For blocks that go back to the system at once instead of to the quarantine (munmap, a
// shrinking sbrk): drops the freed record of `ptr` and restores its shadow, so that a
// later mapping at that address is not reported. ct_alloc_lock not held.
CT_NOINSTR void ct_forget_returned_block(void* ptr, size_t size);

// Conservative auto-free scan. A no-op build is provided where the platform has
// no thread-suspension support, so callers never need a guard.
CT_NOINSTR void ct_autofree_scan_init_once(void);

// True when the scan is enabled and configured to vet individual pointers, so
// __ct_autofree must offer it a pointer before releasing it.
CT_NODISCARD CT_NOINSTR int ct_autofree_scan_checks_pointers(void);

// Non-zero when the scan found `ptr` still reachable and it must not be released.
CT_NODISCARD CT_NOINSTR int ct_autofree_scan_for_ptr(void* ptr, size_t size);

#if defined(__APPLE__)
// A thread of the process, as a scan sees it. Its stack bounds are read before any thread
// is suspended: pthread_from_mach_thread_np takes a lock a suspended thread may hold. Only a
// thread the scan could suspend, or the scanning thread, is read: one that could not be
// suspended has exited, and its stack may already be gone.
struct ct_scan_thread
{
    thread_t port;
    uintptr_t stack_low; // above the guard page
    uintptr_t stack_high;
    bool readable;
};

struct ct_scan_threads
{
    struct ct_scan_thread* items;
    mach_msg_type_number_t count;
    thread_t self;
    uint64_t starts; // the threads started before the list was made
};

// How every scan stops the other threads: it lists them, suspends them, reads their roots,
// checks that the roots it read were all of them, and resumes them.

// Lists the process's threads and their stacks, before any is suspended. False when they
// cannot be listed, with nothing to release.
CT_NODISCARD CT_NOINSTR bool ct_list_threads(struct ct_scan_threads* threads);

// Suspends every listed thread but the calling one.
CT_NOINSTR void ct_suspend_threads(struct ct_scan_threads* threads);

// True when every thread of the process is a listed one, and no thread started since the
// list was made: the roots the scan read are then all the threads' roots, those of a listed
// thread it could not suspend, one that is ending, aside. A scan releases nothing otherwise
// (#209). Calls only the kernel, so it can run while the threads are suspended; a scan calls
// it last, once it has read every root.
CT_NODISCARD CT_NOINSTR bool ct_threads_complete(const struct ct_scan_threads& threads);

// Resumes the threads ct_suspend_threads suspended, and releases the list.
CT_NOINSTR void ct_resume_threads(struct ct_scan_threads* threads);
#endif

#endif // CT_RUNTIME_ALLOC_INTERNAL_H
