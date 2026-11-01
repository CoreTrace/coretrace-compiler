// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_block_index.h"
#include "ct_runtime_internal.h"
#include "ct_runtime_quarantine.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
#include <unordered_map>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// The objects of this file are constructed before the program's and destroyed after them:
// the program's static initializers may already allocate through the table, and
// ct_exit_logging's destructor must come after the destructors of the program's global
// objects and its exit handlers.
#pragma init_seg(lib)

namespace
{
    enum CtAllocKind : unsigned char
    {
        CT_ALLOC_KIND_MALLOC = 0,
        CT_ALLOC_KIND_NEW = 1,
        CT_ALLOC_KIND_NEW_ARRAY = 2,
        CT_ALLOC_KIND_MMAP = 3,
        CT_ALLOC_KIND_SBRK = 4,
        CT_ALLOC_KIND_ALIGNED = 5
    };

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

    struct CtAllocEntry
    {
        size_t size = 0;
        size_t req_size = 0;
        const char* site = nullptr;
        unsigned char state = CT_ENTRY_EMPTY;
        unsigned char kind = CT_ALLOC_KIND_MALLOC;
    };

    // The table and its lock live until the process ends: .CRT$XT terminators, which the
    // CRT runs after the destructors of every static object, may still release blocks, and
    // the leak report runs among them (#159). The lock needs no constructor nor destructor;
    // the table is built on first use, in storage that only the terminator of a DLL's copy
    // destroys, when FreeLibrary unloads the DLL, and built again for the code of the DLL that
    // uses it after that terminator (#227).
    SRWLOCK ct_alloc_lock = SRWLOCK_INIT;

    using CtAllocTable = std::unordered_map<void*, CtAllocEntry>;

    // Set while the table is built. A copy of the runtime that never built it has nothing to
    // report, and building it at process exit would allocate with the loader lock held (#232).
    std::atomic<bool> ct_alloc_table_built{false};

    // ct_alloc_lock held, as for every use of the table.
    CT_NOINSTR CtAllocTable& ct_alloc_table()
    {
        alignas(CtAllocTable) static unsigned char storage[sizeof(CtAllocTable)];
        if (!ct_alloc_table_built.load(std::memory_order_relaxed))
        {
            new (storage) CtAllocTable();
            ct_alloc_table_built.store(true, std::memory_order_release);
        }
        return *std::launder(reinterpret_cast<CtAllocTable*>(storage));
    }

    constexpr size_t kIndexInitialCapacity = size_t{1} << 12;

    // The index below, and the storage it starts with, in the module's image.
    alignas(uintptr_t) unsigned char ct_index_storage[ct_block_index_storage_size(
        kIndexInitialCapacity)];
    ct_block_index ct_index;
    bool ct_index_ready = false;

} // namespace

// The index of the blocks the table records, live or freed, by address, for
// ct_table_lookup_containing (#185). It holds a block while its record is one the lookup sees,
// and changes with the record, under ct_alloc_lock.
CT_NODISCARD CT_NOINSTR ct_block_index* ct_alloc_index()
{
    if (!ct_index_ready)
    {
        ct_block_index_init(&ct_index, ct_index_storage, kIndexInitialCapacity);
        ct_index_ready = true;
    }
    return &ct_index;
}

namespace
{
    // A record the containing lookup sees: a live or freed block, of some size.
    CT_NODISCARD CT_NOINSTR bool ct_entry_is_indexed(const void* ptr, const CtAllocEntry& entry)
    {
        return (entry.state == CT_ENTRY_USED || entry.state == CT_ENTRY_FREED ||
                entry.state == CT_ENTRY_AUTOFREED) &&
               ptr && entry.size != 0;
    }

    // Called before a record changes and after it, so that the index follows it.
    CT_NOINSTR void ct_unindex_entry(const void* ptr, const CtAllocEntry& entry)
    {
        if (ct_entry_is_indexed(ptr, entry))
        {
            ct_block_index_remove(ct_alloc_index(), reinterpret_cast<uintptr_t>(ptr), entry.size);
        }
    }

    CT_NOINSTR void ct_index_entry(const void* ptr, const CtAllocEntry& entry)
    {
        if (ct_entry_is_indexed(ptr, entry))
        {
            // On failure, the index is marked incomplete and the lookup scans the table.
            (void)ct_block_index_add(ct_alloc_index(), reinterpret_cast<uintptr_t>(ptr),
                                     entry.size);
        }
    }

    CT_NODISCARD CT_NOINSTR const char* ct_kind_name(unsigned char kind)
    {
        switch (kind)
        {
        case CT_ALLOC_KIND_MALLOC:
            return "malloc";
        case CT_ALLOC_KIND_NEW:
            return "new";
        case CT_ALLOC_KIND_NEW_ARRAY:
            return "new[]";
        case CT_ALLOC_KIND_MMAP:
            return "mmap";
        case CT_ALLOC_KIND_SBRK:
            return "sbrk";
        case CT_ALLOC_KIND_ALIGNED:
            return "aligned";
        default:
            return "unknown";
        }
    }

    CT_NODISCARD CT_NOINSTR bool ct_is_power_of_two(size_t value)
    {
        return value != 0 && (value & (value - 1)) == 0;
    }

    CT_NOINSTR void ct_track_shadow_alloc(void* ptr, size_t req_size, size_t alloc_size)
    {
        if (!ptr || !ct_is_enabled(CT_FEATURE_SHADOW))
        {
            return;
        }

        const size_t live_size = req_size ? req_size : alloc_size;
        if (live_size != 0)
        {
            ct_shadow_unpoison_range(ptr, live_size);
        }
        if (alloc_size > live_size)
        {
            ct_shadow_poison_range(static_cast<unsigned char*>(ptr) + live_size,
                                   alloc_size - live_size);
        }
    }

    CT_NOINSTR void ct_track_shadow_free(void* ptr, size_t size)
    {
        if (!ptr || !ct_is_enabled(CT_FEATURE_SHADOW) || size == 0)
        {
            return;
        }
        ct_shadow_poison_range(ptr, size);
    }

    // The label of a release, as both runtimes trace it.
    CT_NODISCARD CT_NOINSTR const char* ct_release_label(CtReleaseApi api)
    {
        switch (api)
        {
        case CtReleaseApi::Free:
            return "tracing-free";
        case CtReleaseApi::DeleteArray:
        case CtReleaseApi::DeleteArrayNothrow:
        case CtReleaseApi::DeleteArrayDestroying:
            return "tracing-delete-array";
        case CtReleaseApi::Delete:
        case CtReleaseApi::DeleteNothrow:
        case CtReleaseApi::DeleteDestroying:
            return "tracing-delete";
        }
        return "tracing-free";
    }

    CT_NODISCARD CT_NOINSTR const char* ct_expected_kind_label(CtReleaseApi api)
    {
        switch (api)
        {
        case CtReleaseApi::Free:
            return "malloc/aligned";
        case CtReleaseApi::Delete:
        case CtReleaseApi::DeleteNothrow:
        case CtReleaseApi::DeleteDestroying:
            return "new";
        case CtReleaseApi::DeleteArray:
        case CtReleaseApi::DeleteArrayNothrow:
        case CtReleaseApi::DeleteArrayDestroying:
            return "new[]";
        }
        return "unknown";
    }

    CT_NODISCARD CT_NOINSTR unsigned char ct_default_kind_for_release_api(CtReleaseApi api)
    {
        switch (api)
        {
        case CtReleaseApi::Free:
            return CT_ALLOC_KIND_MALLOC;
        case CtReleaseApi::DeleteArray:
        case CtReleaseApi::DeleteArrayNothrow:
        case CtReleaseApi::DeleteArrayDestroying:
            return CT_ALLOC_KIND_NEW_ARRAY;
        case CtReleaseApi::Delete:
        case CtReleaseApi::DeleteNothrow:
        case CtReleaseApi::DeleteDestroying:
            return CT_ALLOC_KIND_NEW;
        }
        return CT_ALLOC_KIND_MALLOC;
    }

    CT_NODISCARD CT_NOINSTR bool ct_release_api_matches_kind(CtReleaseApi api, unsigned char kind)
    {
        switch (api)
        {
        case CtReleaseApi::Free:
            return kind == CT_ALLOC_KIND_MALLOC || kind == CT_ALLOC_KIND_ALIGNED;
        case CtReleaseApi::Delete:
        case CtReleaseApi::DeleteNothrow:
        case CtReleaseApi::DeleteDestroying:
            return kind == CT_ALLOC_KIND_NEW;
        case CtReleaseApi::DeleteArray:
        case CtReleaseApi::DeleteArrayNothrow:
        case CtReleaseApi::DeleteArrayDestroying:
            return kind == CT_ALLOC_KIND_NEW_ARRAY;
        }
        return false;
    }

    CT_NODISCARD CT_NOINSTR int ct_table_remove_with_state(void* ptr, unsigned char new_state,
                                                           size_t* size_out, size_t* req_size_out,
                                                           const char** site_out,
                                                           unsigned char* kind_out)
    {
        if (!ptr)
        {
            return 0;
        }

        auto it = ct_alloc_table().find(ptr);
        if (it == ct_alloc_table().end())
        {
            return 0;
        }

        CtAllocEntry& entry = it->second;
        if (size_out)
        {
            *size_out = entry.size;
        }
        if (req_size_out)
        {
            *req_size_out = entry.req_size;
        }
        if (site_out)
        {
            *site_out = entry.site;
        }
        if (kind_out)
        {
            *kind_out = entry.kind;
        }

        if (entry.state == CT_ENTRY_FREED || entry.state == CT_ENTRY_AUTOFREED)
        {
            return -1;
        }

        entry.state = new_state;
        return 1;
    }

    CT_NOINSTR void ct_release_autofree_memory(void* ptr, unsigned char kind)
    {
        if (!ptr)
        {
            return;
        }

        switch (kind)
        {
        case CT_ALLOC_KIND_NEW:
            ::operator delete(ptr);
            return;
        case CT_ALLOC_KIND_NEW_ARRAY:
            ::operator delete[](ptr);
            return;
        case CT_ALLOC_KIND_MMAP:
            (void)VirtualFree(ptr, 0, MEM_RELEASE);
            return;
        case CT_ALLOC_KIND_ALIGNED:
            _aligned_free(ptr);
            return;
        default:
            std::free(ptr);
            return;
        }
    }

    CT_NOINSTR void ct_release_by_called_api(void* ptr, CtReleaseApi api,
                                             unsigned char recorded_kind)
    {
        if (!ptr)
        {
            return;
        }

        switch (api)
        {
        case CtReleaseApi::Free:
            if (recorded_kind == CT_ALLOC_KIND_ALIGNED)
            {
                _aligned_free(ptr);
                return;
            }
            std::free(ptr);
            return;
        case CtReleaseApi::Delete:
            ::operator delete(ptr);
            return;
        case CtReleaseApi::DeleteArray:
            ::operator delete[](ptr);
            return;
        case CtReleaseApi::DeleteNothrow:
            ::operator delete(ptr, std::nothrow);
            return;
        case CtReleaseApi::DeleteArrayNothrow:
            ::operator delete[](ptr, std::nothrow);
            return;
        case CtReleaseApi::DeleteDestroying:
            ::operator delete(ptr);
            return;
        case CtReleaseApi::DeleteArrayDestroying:
            ::operator delete[](ptr);
            return;
        }
    }

    CT_NODISCARD CT_NOINSTR int ct_remove_for_release(void* ptr, unsigned char new_state,
                                                      size_t* size_out, const char** site_out,
                                                      unsigned char* kind_out);

    // The release function matching an allocation kind, for blocks the runtime releases
    // itself (auto-free).
    CT_NODISCARD CT_NOINSTR CtReleaseApi ct_release_api_for_kind(unsigned char kind)
    {
        switch (kind)
        {
        case CT_ALLOC_KIND_NEW:
            return CtReleaseApi::Delete;
        case CT_ALLOC_KIND_NEW_ARRAY:
            return CtReleaseApi::DeleteArray;
        default:
            return CtReleaseApi::Free;
        }
    }

    // For mappings, which go back to the system at once instead of to the quarantine: drops
    // the freed record of `ptr` and restores its shadow, so that a later mapping at that
    // address is not reported. ct_alloc_lock not held.
    CT_NOINSTR void ct_forget_returned_block(void* ptr, size_t size)
    {
        ct_lock_acquire();
        ct_table_forget_freed(ptr);
        ct_lock_release();
        ct_shadow_unpoison_range(ptr, size);
    }

    // `trace` false leaves the release out of the trace, for realloc, which traces the resize
    // instead; its diagnostics stay.
    CT_NOINSTR void ct_release_tracked_pointer(void* ptr, CtReleaseApi api, const char* site,
                                               bool trace = true)
    {
        ct_init_env_once();

        const char* label = ct_release_label(api);
        unsigned char kind = ct_default_kind_for_release_api(api);
        // A release the runtime makes for itself (#197).
        if (ct_in_runtime_scope())
        {
            ct_release_by_called_api(ptr, api, kind);
            return;
        }
        CtRuntimeScope scope;
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            ct_release_by_called_api(ptr, api, kind);
            return;
        }

        if (!ptr)
        {
            ct_log_release_null(label);
            ct_release_by_called_api(ptr, api, kind);
            return;
        }

        size_t size = 0;
        const char* alloc_site = nullptr;

        ct_lock_acquire();
        const int found = ct_remove_for_release(ptr, CT_ENTRY_FREED, &size, &alloc_site, &kind);
        ct_lock_release();

        if (found == -1)
        {
            ct_log_double_free(label, ptr, site, alloc_site);
            return;
        }
        if (found == 0)
        {
            // A block the runtime did not track, such as one the C++ library allocated: its
            // release is traced like any other, not reported.
            if (trace && ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
            {
                ct_log_release_unknown(label, ptr);
            }
            ct_release_by_called_api(ptr, api, kind);
            return;
        }

        ct_track_shadow_free(ptr, size);
        if (trace && ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            ct_log_release(label, ptr, size, ct_kind_name(kind), alloc_site);
        }
        if (!ct_release_api_matches_kind(api, kind))
        {
            ct_log_deallocator_mismatch(label, ptr, ct_expected_kind_label(api), ct_kind_name(kind),
                                        alloc_site);
        }
        ct_quarantine_push({ptr, size, static_cast<unsigned char>(api), kind});
    }

    CT_NODISCARD CT_NOINSTR DWORD ct_translate_page_protection(int prot)
    {
        constexpr int kProtRead = 0x1;
        constexpr int kProtWrite = 0x2;
        constexpr int kProtExec = 0x4;

        const bool can_read = (prot & kProtRead) != 0;
        const bool can_write = (prot & kProtWrite) != 0;
        const bool can_exec = (prot & kProtExec) != 0;

        if (can_exec && can_write)
        {
            return PAGE_EXECUTE_READWRITE;
        }
        if (can_exec && can_read)
        {
            return PAGE_EXECUTE_READ;
        }
        if (can_exec)
        {
            return PAGE_EXECUTE;
        }
        if (can_write)
        {
            return PAGE_READWRITE;
        }
        if (can_read)
        {
            return PAGE_READONLY;
        }
        return PAGE_NOACCESS;
    }

    CT_NOINSTR void ct_track_alloc(void* ptr, size_t req_size, size_t alloc_size, const char* site,
                                   unsigned char kind)
    {
        // The table allocates its records.
        CtRuntimeScope scope;
        ct_lock_acquire();
        (void)ct_table_insert(ptr, req_size, alloc_size, site, kind);
        ct_lock_release();

        ct_track_shadow_alloc(ptr, req_size, alloc_size);
    }

    // An allocation, traced under `label`, as the POSIX runtime traces it.
    CT_NODISCARD CT_NOINSTR void* ct_record_alloc(void* ptr, size_t req_size, size_t alloc_size,
                                                  const char* site, unsigned char kind,
                                                  const char* label)
    {
        // An allocation the runtime makes for itself is not the program's (#197).
        if (!ptr || ct_in_runtime_scope())
        {
            return ptr;
        }

        CtRuntimeScope scope;
        ct_track_alloc(ptr, req_size, alloc_size, site, kind);
        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            ct_log_alloc_details(label, "reachable", req_size, alloc_size, ptr, site,
                                 CTColor::Yellow, CTLevel::Info);
        }
        return ptr;
    }

    // An allocation whose result the program never uses, logged as the POSIX runtime logs
    // it under `label`. The instrumentation releases it right after the call when auto-free
    // is enabled.
    CT_NODISCARD CT_NOINSTR void* ct_record_unreachable_alloc(void* ptr, size_t size,
                                                              const char* site, unsigned char kind,
                                                              const char* label)
    {
        if (!ptr || ct_in_runtime_scope())
        {
            return ptr;
        }

        CtRuntimeScope scope;
        ct_track_alloc(ptr, size, size, site, kind);
        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            ct_log_alloc_details(label, "unreachable", size, size, ptr, site, CTColor::Yellow,
                                 CTLevel::Warn);
        }
        return ptr;
    }

    CT_NOINSTR void ct_record_realloc(void* old_ptr, void* new_ptr, size_t size, const char* site)
    {
        CtRuntimeScope scope;
        if (!new_ptr)
        {
            return;
        }

        ct_lock_acquire();
        if (old_ptr && old_ptr != new_ptr)
        {
            (void)ct_table_remove_with_state(old_ptr, CT_ENTRY_FREED, nullptr, nullptr, nullptr,
                                             nullptr);
        }

        (void)ct_table_insert(new_ptr, size, size, site, CT_ALLOC_KIND_MALLOC);
        ct_lock_release();

        ct_track_shadow_alloc(new_ptr, size, size);
    }

    CT_NODISCARD CT_NOINSTR int ct_remove_for_release(void* ptr, unsigned char new_state,
                                                      size_t* size_out, const char** site_out,
                                                      unsigned char* kind_out)
    {
        size_t req_size = 0;
        return ct_table_remove_with_state(ptr, new_state, size_out, &req_size, site_out, kind_out);
    }

    CT_NOINSTR void ct_autofree_impl(void* ptr)
    {
        ct_init_env_once();
        if (ct_in_runtime_scope())
        {
            return;
        }
        CtRuntimeScope scope;
        if (!ct_is_enabled(CT_FEATURE_ALLOC) || !ct_is_enabled(CT_FEATURE_AUTOFREE))
        {
            return;
        }
        if (!ptr)
        {
            ct_log_autofree_skipped(ptr, nullptr);
            return;
        }

        size_t size = 0;
        const char* site = nullptr;
        unsigned char kind = CT_ALLOC_KIND_MALLOC;

        ct_lock_acquire();
        const int found = ct_remove_for_release(ptr, CT_ENTRY_AUTOFREED, &size, &site, &kind);
        ct_lock_release();

        if (found <= 0)
        {
            ct_log_autofree_skipped(ptr, found == 0 ? "unknown" : "already freed");
            return;
        }

        ct_log_autofree(ptr, size, site);

        if (kind == CT_ALLOC_KIND_MMAP)
        {
            ct_forget_returned_block(ptr, size);
            ct_release_autofree_memory(ptr, kind);
            return;
        }
        ct_track_shadow_free(ptr, size);
        ct_quarantine_push(
            {ptr, size, static_cast<unsigned char>(ct_release_api_for_kind(kind)), kind});
    }

    // Constructed in this file's init_seg, so destroyed after the program's static objects
    // and before its terminators. From there on, releases do not log: the logger's state may
    // already be destroyed.
    struct CtExitLogging
    {
        // User-provided, so that the object is initialized dynamically, in this file's
        // init_seg: clang registers the destructor of a constant-initialized object from
        // .CRT$XCU, with the program's own objects.
        CT_NOINSTR CtExitLogging() {}

        CT_NOINSTR ~CtExitLogging()
        {
            ct_disable_logging();
        }
    };

    CtExitLogging ct_exit_logging;
} // namespace

// Runs at process exit, from a TLS callback of the executable (ct_report_leaks_tls_callback):
// after the destructors of the program's static objects, its exit handlers, its terminators
// and the exit-time code of every DLL, which release blocks too (#187). In a DLL's copy of the
// runtime, after the DLL's own exit-time code, from a terminator of the DLL
// (ct_report_dll_leaks_terminator), or from its TLS callback when the DLL ended the process with
// exit() (#232). The loader lock is held by then, and ExitProcess has killed every other thread,
// maybe while it held a lock that nothing will release: the report only tries the allocation
// table's lock, allocates nothing, loads nothing, and writes with the writers that take no lock
// of the runtime, logging being off. They write with the C runtime's _write, which takes the lock
// of the standard error's descriptor.
CT_NOINSTR bool ct_report_leaks(void)
{
    CtRuntimeScope scope;
    if (!TryAcquireSRWLockExclusive(&ct_alloc_lock))
    {
        ct_write_prefix_nolock(CTLevel::Warn);
        ct_write_cstr("ct: leak report skipped: another thread holds the allocation table\n");
        return false;
    }
    if (!ct_alloc_table_built.load(std::memory_order_acquire))
    {
        ReleaseSRWLockExclusive(&ct_alloc_lock);
        return true;
    }

    size_t count = 0;
    for (const auto& [ptr, entry] : ct_alloc_table())
    {
        count += entry.state == CT_ENTRY_USED ? 1 : 0;
    }
    if (count != 0)
    {
        ct_write_prefix_nolock(CTLevel::Error);
        ct_write_cstr("ct: leaks detected count=");
        ct_write_dec(count);
        ct_write_cstr("\n");

        size_t reported = 0;
        for (const auto& [ptr, entry] : ct_alloc_table())
        {
            if (entry.state != CT_ENTRY_USED)
            {
                continue;
            }
            ct_write_prefix_nolock(CTLevel::Warn);
            ct_write_cstr("ct: leak ptr=");
            ct_write_hex(reinterpret_cast<uintptr_t>(ptr));
            ct_write_cstr(" size=");
            ct_write_dec(entry.size);
            ct_write_cstr(" alloc_site=");
            ct_write_cstr(ct_site_name(entry.site));
            ct_write_cstr("\n");

            if (++reported >= 32)
            {
                ct_write_prefix_nolock(CTLevel::Warn);
                ct_write_cstr("ct: leak list truncated\n");
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&ct_alloc_lock);
    return true;
}

CT_NOINSTR void ct_lock_acquire(void)
{
    AcquireSRWLockExclusive(&ct_alloc_lock);
}

CT_NOINSTR void ct_lock_release(void)
{
    ReleaseSRWLockExclusive(&ct_alloc_lock);
}

CT_NODISCARD CT_NOINSTR int ct_table_insert(void* ptr, size_t req_size, size_t size,
                                            const char* site, unsigned char kind)
{
    if (!ptr)
    {
        return 0;
    }

    try
    {
        auto& entry = ct_alloc_table()[ptr];
        // The record may hold a freed block of this address, or a live one of another size.
        ct_unindex_entry(ptr, entry);
        entry.size = size;
        entry.req_size = req_size;
        entry.site = site;
        entry.state = CT_ENTRY_USED;
        entry.kind = kind;
        ct_index_entry(ptr, entry);
        return 1;
    }
    catch (...)
    {
        return 0;
    }
}

// The allocator's side of the quarantine, see ct_runtime_quarantine.h.
CT_NOINSTR void ct_table_forget_freed(void* ptr)
{
    auto it = ct_alloc_table().find(ptr);
    if (it != ct_alloc_table().end() &&
        (it->second.state == CT_ENTRY_FREED || it->second.state == CT_ENTRY_AUTOFREED))
    {
        ct_unindex_entry(it->first, it->second);
        ct_alloc_table().erase(it);
    }
}

CT_NOINSTR void ct_quarantine_release(const ct_quarantine_item& item)
{
    ct_shadow_unpoison_range(item.ptr, item.size);
    ct_release_by_called_api(item.ptr, static_cast<CtReleaseApi>(item.api), item.kind);
}

CT_NODISCARD CT_NOINSTR int ct_table_remove(void* ptr, size_t* size_out, size_t* req_size_out,
                                            const char** site_out)
{
    return ct_table_remove_with_state(ptr, CT_ENTRY_FREED, size_out, req_size_out, site_out,
                                      nullptr);
}

CT_NODISCARD CT_NOINSTR int ct_table_lookup(const void* ptr, size_t* size_out, size_t* req_size_out,
                                            const char** site_out, unsigned char* state_out)
{
    if (!ptr)
    {
        return 0;
    }

    auto it = ct_alloc_table().find(const_cast<void*>(ptr));
    if (it == ct_alloc_table().end())
    {
        return 0;
    }

    const CtAllocEntry& entry = it->second;
    if (size_out)
    {
        *size_out = entry.size;
    }
    if (req_size_out)
    {
        *req_size_out = entry.req_size;
    }
    if (site_out)
    {
        *site_out = entry.site;
    }
    if (state_out)
    {
        *state_out = entry.state;
    }
    return 1;
}

// ct_table_lookup_containing over every record, for when the index misses blocks.
CT_NODISCARD CT_NOINSTR static int ct_table_scan_containing(const void* ptr, void** base_out,
                                                            size_t* size_out, size_t* req_size_out,
                                                            const char** site_out,
                                                            unsigned char* state_out)
{
    const uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
    for (const auto& [base_ptr, entry] : ct_alloc_table())
    {
        if (entry.state != CT_ENTRY_USED && entry.state != CT_ENTRY_FREED &&
            entry.state != CT_ENTRY_AUTOFREED)
        {
            continue;
        }
        if (!base_ptr || entry.size == 0)
        {
            continue;
        }

        const uintptr_t base = reinterpret_cast<uintptr_t>(base_ptr);
        if (value < base || (value - base) >= entry.size)
        {
            continue;
        }

        if (base_out)
        {
            *base_out = base_ptr;
        }
        if (size_out)
        {
            *size_out = entry.size;
        }
        if (req_size_out)
        {
            *req_size_out = entry.req_size;
        }
        if (site_out)
        {
            *site_out = entry.site;
        }
        if (state_out)
        {
            *state_out = entry.state;
        }
        return 1;
    }

    return 0;
}

CT_NODISCARD CT_NOINSTR int ct_table_lookup_containing(const void* ptr, void** base_out,
                                                       size_t* size_out, size_t* req_size_out,
                                                       const char** site_out,
                                                       unsigned char* state_out)
{
    if (!ptr)
    {
        return 0;
    }

    const ct_block_index* index = ct_alloc_index();
    if (index->incomplete)
    {
        return ct_table_scan_containing(ptr, base_out, size_out, req_size_out, site_out, state_out);
    }

    uintptr_t base = 0;
    size_t size = 0;
    if (!ct_block_index_find(index, reinterpret_cast<uintptr_t>(ptr), &base, &size))
    {
        return 0;
    }
    const auto it = ct_alloc_table().find(reinterpret_cast<void*>(base));
    if (it == ct_alloc_table().end())
    {
        return 0;
    }

    const CtAllocEntry& entry = it->second;
    if (base_out)
    {
        *base_out = it->first;
    }
    if (size_out)
    {
        *size_out = entry.size;
    }
    if (req_size_out)
    {
        *req_size_out = entry.req_size;
    }
    if (site_out)
    {
        *site_out = entry.site;
    }
    if (state_out)
    {
        *state_out = entry.state;
    }
    return 1;
}

extern "C"
{
    CT_NOINSTR void __ct_free(void* ptr, const char* site);

    CT_NODISCARD CT_NOINSTR void* __ct_malloc(size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return std::malloc(size);
        }
        return ct_record_alloc(std::malloc(size), size, size, site, CT_ALLOC_KIND_MALLOC,
                               "tracing-malloc");
    }

    CT_NODISCARD CT_NOINSTR void* __ct_malloc_unreachable(size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return std::malloc(size);
        }
        return ct_record_unreachable_alloc(std::malloc(size), size, site, CT_ALLOC_KIND_MALLOC,
                                           "tracing-malloc-unreachable");
    }

    CT_NODISCARD CT_NOINSTR void* __ct_calloc(size_t count, size_t size, const char* site)
    {
        ct_init_env_once();
        const size_t total = count * size;
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return std::calloc(count, size);
        }
        return ct_record_alloc(std::calloc(count, size), total, total, site, CT_ALLOC_KIND_MALLOC,
                               "tracing-calloc");
    }

    CT_NODISCARD CT_NOINSTR void* __ct_calloc_unreachable(size_t count, size_t size,
                                                          const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return std::calloc(count, size);
        }
        return ct_record_unreachable_alloc(std::calloc(count, size), count * size, site,
                                           CT_ALLOC_KIND_MALLOC, "tracing-calloc-unreachable");
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new(size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return ::operator new(size);
        }
        return ct_record_alloc(::operator new(size), size, size, site, CT_ALLOC_KIND_NEW,
                               "tracing-new");
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_unreachable(size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return ::operator new(size);
        }
        return ct_record_unreachable_alloc(::operator new(size), size, site, CT_ALLOC_KIND_NEW,
                                           "tracing-new-unreachable");
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_array(size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return ::operator new[](size);
        }
        return ct_record_alloc(::operator new[](size), size, size, site, CT_ALLOC_KIND_NEW_ARRAY,
                               "tracing-new-array");
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_array_unreachable(size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return ::operator new[](size);
        }
        return ct_record_unreachable_alloc(::operator new[](size), size, site,
                                           CT_ALLOC_KIND_NEW_ARRAY,
                                           "tracing-new-array-unreachable");
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_nothrow(size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return ::operator new(size, std::nothrow);
        }
        return ct_record_alloc(::operator new(size, std::nothrow), size, size, site,
                               CT_ALLOC_KIND_NEW, "tracing-new");
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_nothrow_unreachable(size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return ::operator new(size, std::nothrow);
        }
        return ct_record_unreachable_alloc(::operator new(size, std::nothrow), size, site,
                                           CT_ALLOC_KIND_NEW, "tracing-new-unreachable");
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_array_nothrow(size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return ::operator new[](size, std::nothrow);
        }
        return ct_record_alloc(::operator new[](size, std::nothrow), size, size, site,
                               CT_ALLOC_KIND_NEW_ARRAY, "tracing-new-array");
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_array_nothrow_unreachable(size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return ::operator new[](size, std::nothrow);
        }
        return ct_record_unreachable_alloc(::operator new[](size, std::nothrow), size, site,
                                           CT_ALLOC_KIND_NEW_ARRAY,
                                           "tracing-new-array-unreachable");
    }

    CT_NODISCARD CT_NOINSTR void* __ct_realloc(void* ptr, size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC) || ct_in_runtime_scope())
        {
            return std::realloc(ptr, size);
        }

        size_t old_size = 0;
        size_t old_req_size = 0;
        unsigned char state = CT_ENTRY_EMPTY;
        if (ptr)
        {
            ct_lock_acquire();
            (void)ct_table_lookup(ptr, &old_size, &old_req_size, nullptr, &state);
            ct_lock_release();
        }
        // A block recorded as freed is in the quarantine, which will release it: the C
        // library must not release it first.
        if (state == CT_ENTRY_FREED || state == CT_ENTRY_AUTOFREED)
        {
            ct_log_realloc_freed(ptr, site);
            return nullptr;
        }

        // As on POSIX, one trace for the resize, whichever way it goes. A tracked block is
        // never resized in place, nor released by the C library: the contents move to a new
        // block and the old one goes to the quarantine. Other blocks go through the C
        // library's realloc.
        void* new_ptr = nullptr;
        const char* status = "updated";
        if (!ptr)
        {
            new_ptr = std::malloc(size);
            if (new_ptr)
            {
                ct_track_alloc(new_ptr, size, size, site, CT_ALLOC_KIND_MALLOC);
            }
            status = "allocated";
        }
        else if (size == 0)
        {
            ct_release_tracked_pointer(ptr, CtReleaseApi::Free, site, /*trace=*/false);
            status = "freed";
        }
        else if (state == CT_ENTRY_USED)
        {
            new_ptr = std::malloc(size);
            if (new_ptr)
            {
                ct_track_alloc(new_ptr, size, size, site, CT_ALLOC_KIND_MALLOC);
                std::memcpy(new_ptr, ptr, old_req_size < size ? old_req_size : size);
                ct_release_tracked_pointer(ptr, CtReleaseApi::Free, site, /*trace=*/false);
                status = "moved";
            }
        }
        else
        {
            new_ptr = std::realloc(ptr, size);
            if (new_ptr)
            {
                ct_record_realloc(ptr, new_ptr, size, site);
                status = new_ptr == ptr ? "in-place" : "moved";
            }
        }
        if (!new_ptr && size > 0)
        {
            status = "failed";
        }

        if (ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            ct_log_realloc_details("tracing-realloc", status, old_req_size, old_size, ptr, size,
                                   new_ptr ? size : 0, new_ptr, site, CTColor::Yellow);
        }
        return new_ptr;
    }

    CT_NODISCARD CT_NOINSTR int __ct_posix_memalign(void** out, size_t align, size_t size,
                                                    const char* site)
    {
        ct_init_env_once();
        if (!out || !ct_is_power_of_two(align) || align < sizeof(void*))
        {
            return EINVAL;
        }

        void* ptr = _aligned_malloc(size, align);
        if (!ptr)
        {
            return errno ? errno : ENOMEM;
        }

        *out = ptr;
        if (ct_is_enabled(CT_FEATURE_ALLOC))
        {
            (void)ct_record_alloc(ptr, size, size, site, CT_ALLOC_KIND_ALIGNED,
                                  "tracing-posix-memalign");
        }
        return 0;
    }

    CT_NODISCARD CT_NOINSTR void* __ct_aligned_alloc(size_t align, size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_power_of_two(align) || align == 0)
        {
            errno = EINVAL;
            return nullptr;
        }

        void* ptr = _aligned_malloc(size, align);
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return ptr;
        }
        return ct_record_alloc(ptr, size, size, site, CT_ALLOC_KIND_ALIGNED,
                               "tracing-aligned-alloc");
    }

    CT_NODISCARD CT_NOINSTR void* __ct_mmap(void* addr, size_t len, int prot, int flags, int fd,
                                            size_t offset, const char* site)
    {
        ct_init_env_once();
        (void)flags;
        if (fd != -1 || offset != 0 || len == 0)
        {
            errno = ENOSYS;
            return reinterpret_cast<void*>(-1);
        }

        void* ptr =
            VirtualAlloc(addr, len, MEM_COMMIT | MEM_RESERVE, ct_translate_page_protection(prot));
        if (!ptr)
        {
            errno = ENOMEM;
            return reinterpret_cast<void*>(-1);
        }

        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return ptr;
        }
        return ct_record_alloc(ptr, len, len, site, CT_ALLOC_KIND_MMAP, "tracing-mmap");
    }

    CT_NODISCARD CT_NOINSTR int __ct_munmap(void* addr, size_t len, const char* site)
    {
        ct_init_env_once();
        (void)len;

        size_t size = 0;
        const char* alloc_site = nullptr;
        unsigned char kind = CT_ALLOC_KIND_MMAP;
        int found = 0;

        if (ct_is_enabled(CT_FEATURE_ALLOC))
        {
            ct_lock_acquire();
            found = ct_remove_for_release(addr, CT_ENTRY_FREED, &size, &alloc_site, &kind);
            ct_lock_release();
            if (found > 0)
            {
                ct_forget_returned_block(addr, size);
            }
        }

        const BOOL ok = addr ? VirtualFree(addr, 0, MEM_RELEASE) : TRUE;
        if (!ok)
        {
            errno = EINVAL;
            return -1;
        }

        if (ct_is_enabled(CT_FEATURE_ALLOC) && ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            if (found > 0)
            {
                ct_log_release("tracing-munmap", addr, size, ct_kind_name(kind), alloc_site);
            }
            else
            {
                ct_log_release_unknown("tracing-munmap", addr);
            }
        }
        return 0;
    }

    CT_NODISCARD CT_NOINSTR void* __ct_sbrk(size_t incr, const char* site)
    {
        ct_init_env_once();
        (void)incr;
        (void)site;
        errno = ENOSYS;
        ct_log(CTLevel::Warn, "ct: sbrk is not supported on Windows\n");
        return reinterpret_cast<void*>(-1);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_brk(void* addr, const char* site)
    {
        ct_init_env_once();
        (void)addr;
        (void)site;
        errno = ENOSYS;
        ct_log(CTLevel::Warn, "ct: brk is not supported on Windows\n");
        return reinterpret_cast<void*>(-1);
    }

    CT_NOINSTR void __ct_autofree(void* ptr)
    {
        ct_autofree_impl(ptr);
    }

    CT_NOINSTR void __ct_autofree_munmap(void* ptr)
    {
        ct_autofree_impl(ptr);
    }

    CT_NOINSTR void __ct_autofree_sbrk(void* ptr)
    {
        ct_autofree_impl(ptr);
    }

    CT_NOINSTR void __ct_autofree_delete(void* ptr)
    {
        ct_autofree_impl(ptr);
    }

    CT_NOINSTR void __ct_autofree_delete_array(void* ptr)
    {
        ct_autofree_impl(ptr);
    }

    CT_NOINSTR void __ct_free(void* ptr, const char* site)
    {
        ct_release_tracked_pointer(ptr, CtReleaseApi::Free, site);
    }

    CT_NOINSTR void __ct_delete(void* ptr, const char* site)
    {
        ct_release_tracked_pointer(ptr, CtReleaseApi::Delete, site);
    }

    CT_NOINSTR void __ct_delete_array(void* ptr, const char* site)
    {
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteArray, site);
    }

    CT_NOINSTR void __ct_delete_nothrow(void* ptr, const char* site)
    {
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteNothrow, site);
    }

    CT_NOINSTR void __ct_delete_array_nothrow(void* ptr, const char* site)
    {
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteArrayNothrow, site);
    }

    CT_NOINSTR void __ct_delete_destroying(void* ptr, const char* site)
    {
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteDestroying, site);
    }

    CT_NOINSTR void __ct_delete_array_destroying(void* ptr, const char* site)
    {
        ct_release_tracked_pointer(ptr, CtReleaseApi::DeleteArrayDestroying, site);
    }

    // The report runs from a TLS callback (ct_report_leaks_tls_callback), which needs no
    // scheduling.
    CT_NOINSTR void __ct_schedule_leak_report(void) {}
}

namespace
{
    // The report of a DLL's copy of the runtime, which has a table of its own, once: when the
    // process exits, after the DLL's exit-time code, which releases blocks too (#232). When
    // FreeLibrary unloads the DLL, the process goes on.
    std::atomic<bool> ct_dll_reported{false};
    // Set by the TLS callback when the DLL that holds this copy detaches.
    std::atomic<bool> ct_dll_detaching{false};
    // Set by the terminator when it runs before the DLL detaches: in the executable, from its C
    // runtime's exit(); in a DLL, from the DLL's own exit(), whose C runtime runs the DLL's
    // exit-time code then, and not again at detach.
    std::atomic<bool> ct_terminators_ran{false};

    CT_NOINSTR void ct_report_dll_leaks_once()
    {
        if (ct_process_shutdown() == CtProcessShutdown::InProgress &&
            !ct_dll_reported.exchange(true, std::memory_order_acq_rel))
        {
            (void)ct_report_leaks();
        }
    }

    // The executable detaches only when the process exits: it reports then. A DLL reports here
    // when its own exit() has already run its exit-time code, terminators included. A DLL's TLS
    // callbacks run before its exit-time code otherwise.
    CT_NOINSTR void NTAPI ct_report_leaks_at_exit(PVOID module, DWORD reason, PVOID)
    {
        if (reason != DLL_PROCESS_DETACH)
        {
            return;
        }
        if (module == GetModuleHandleW(nullptr))
        {
            (void)ct_report_leaks();
            return;
        }
        ct_dll_detaching.store(true, std::memory_order_release);
        if (ct_terminators_ran.load(std::memory_order_acquire))
        {
            ct_report_dll_leaks_once();
        }
    }

    // Frees what this copy of the runtime holds for the blocks of its DLL when FreeLibrary unloads
    // the DLL, from the terminator below: the blocks in its quarantine, which the program had
    // released, the quarantine's queue, what its block index grew into, and its allocation table.
    // The DLL's code has all run by then: its atexit handlers, the destructors of its global
    // objects, its destructor functions and its own terminators. Only the C runtime's teardown
    // comes after, which calls no function of the runtime. The blocks that the DLL still holds stay
    // allocated, without their records (#227).
    CT_NOINSTR void ct_release_alloc_state()
    {
        // The table and the quarantine release blocks with ct_alloc_lock held, which may reach the
        // runtime's entry points (#197).
        CtRuntimeScope scope;
        if (!ct_alloc_table_built.load(std::memory_order_acquire))
        {
            return;
        }
        ct_quarantine_release_all();
        ct_lock_acquire();
        ct_block_index_destroy(&ct_index);
        ct_block_index_init(&ct_index, ct_index_storage, kIndexInitialCapacity);
        ct_alloc_table().~CtAllocTable();
        ct_alloc_table_built.store(false, std::memory_order_release);
        ct_lock_release();
    }

    // A DLL's C runtime runs its .CRT$XTY terminators after its atexit handlers, the destructors
    // of its global objects and its destructor functions, when FreeLibrary unloads it and when the
    // process exits. When FreeLibrary unloads it, the copy then frees its allocation state; it
    // keeps it when the process exits, for the leak report, and while the DLL's own exit() runs.
    CT_NOINSTR void ct_report_dll_leaks_at_exit()
    {
        if (!ct_dll_detaching.load(std::memory_order_acquire))
        {
            ct_terminators_ran.store(true, std::memory_order_release);
            return;
        }
        ct_report_dll_leaks_once();
        if (ct_dll_unloading_dynamically())
        {
            ct_release_alloc_state();
        }
    }
} // namespace

// A TLS callback of the executable runs at process exit after the exit-time code of every
// DLL, whether the program returns from main, calls exit() or ExitProcess: the terminators
// of the C runtime run before the DLLs, and not at all on ExitProcess (#187). .CRT$XLY
// comes after the sections of the program's own TLS callbacks.
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:ct_report_leaks_tls_callback")
#pragma section(".CRT$XLY", read)
extern "C" __declspec(allocate(".CRT$XLY")) __attribute__((used))
const PIMAGE_TLS_CALLBACK ct_report_leaks_tls_callback = ct_report_leaks_at_exit;

// The terminator of a DLL's copy. .CRT$XTY comes after .CRT$XTU, where programs place theirs.
#pragma comment(linker, "/INCLUDE:ct_report_dll_leaks_terminator")
#pragma section(".CRT$XTY", read)
extern "C" __declspec(allocate(".CRT$XTY"))
__attribute__((used)) void (*const ct_report_dll_leaks_terminator)() = ct_report_dll_leaks_at_exit;
