// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_internal.h"

#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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

    struct CtAllocEntry
    {
        size_t size = 0;
        size_t req_size = 0;
        const char* site = nullptr;
        unsigned char state = CT_ENTRY_EMPTY;
        unsigned char kind = CT_ALLOC_KIND_MALLOC;
    };

    std::mutex ct_alloc_mutex;
    std::unordered_map<void*, CtAllocEntry> ct_alloc_table;

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
            ct_shadow_poison_range(static_cast<unsigned char*>(ptr) + live_size, alloc_size - live_size);
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

    CT_NOINSTR void ct_log_alloc_event(const char* action, void* ptr, size_t size, const char* site,
                                       unsigned char kind)
    {
        if (!ct_is_enabled(CT_FEATURE_ALLOC_TRACE))
        {
            return;
        }

        ct_log(CTLevel::Info, "ct: {} ptr={:p} size={} kind={} site={}\n", action, ptr, size,
               ct_kind_name(kind), ct_site_name(site));
    }

    CT_NOINSTR void ct_log_skip_event(const char* action, void* ptr, const char* reason)
    {
        ct_log(CTLevel::Warn, "ct: {} skipped ptr={:p} ({})\n", action, ptr, reason);
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

        auto it = ct_alloc_table.find(ptr);
        if (it == ct_alloc_table.end())
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

    CT_NOINSTR void ct_release_memory(void* ptr, unsigned char kind)
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

    CT_NOINSTR void ct_release_unknown_delete(void* ptr, bool is_array)
    {
        if (!ptr)
        {
            return;
        }

        if (is_array)
        {
            ::operator delete[](ptr);
        }
        else
        {
            ::operator delete(ptr);
        }
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

    CT_NODISCARD CT_NOINSTR void* ct_record_alloc(void* ptr, size_t req_size, size_t alloc_size,
                                                  const char* site, unsigned char kind)
    {
        if (!ptr)
        {
            return ptr;
        }

        ct_lock_acquire();
        (void)ct_table_insert(ptr, req_size, alloc_size, site, kind);
        ct_lock_release();

        ct_track_shadow_alloc(ptr, req_size, alloc_size);
        ct_log_alloc_event("alloc", ptr, req_size ? req_size : alloc_size, site, kind);
        return ptr;
    }

    CT_NOINSTR void ct_record_realloc(void* old_ptr, void* new_ptr, size_t size, const char* site)
    {
        if (!new_ptr)
        {
            return;
        }

        ct_lock_acquire();
        if (old_ptr && old_ptr != new_ptr)
        {
            (void)ct_table_remove_with_state(old_ptr, CT_ENTRY_FREED, nullptr, nullptr, nullptr, nullptr);
        }

        auto& entry = ct_alloc_table[new_ptr];
        entry.size = size;
        entry.req_size = size;
        entry.site = site;
        entry.state = CT_ENTRY_USED;
        entry.kind = CT_ALLOC_KIND_MALLOC;
        ct_lock_release();

        ct_track_shadow_alloc(new_ptr, size, size);
        ct_log_alloc_event("realloc", new_ptr, size, site, CT_ALLOC_KIND_MALLOC);
    }

    CT_NODISCARD CT_NOINSTR int ct_remove_for_release(void* ptr, unsigned char new_state,
                                                      size_t* size_out, const char** site_out,
                                                      unsigned char* kind_out)
    {
        size_t req_size = 0;
        return ct_table_remove_with_state(ptr, new_state, size_out, &req_size, site_out, kind_out);
    }

    CT_NOINSTR void ct_autofree_impl(void* ptr, bool is_array)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC) || !ct_is_enabled(CT_FEATURE_AUTOFREE))
        {
            return;
        }
        if (!ptr)
        {
            ct_log_skip_event("auto-free", ptr, "null");
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
            ct_log_skip_event("auto-free", ptr, found == 0 ? "unknown" : "already freed");
            return;
        }

        ct_track_shadow_free(ptr, size);
        ct_log(CTLevel::Warn, "ct: auto-free ptr={:p} size={} site={}\n", ptr, size,
               ct_site_name(site));

        if (kind == CT_ALLOC_KIND_NEW_ARRAY || (kind == CT_ALLOC_KIND_NEW && is_array))
        {
            ::operator delete[](ptr);
            return;
        }
        if (kind == CT_ALLOC_KIND_NEW || kind == CT_ALLOC_KIND_NEW_ARRAY || kind == CT_ALLOC_KIND_ALIGNED ||
            kind == CT_ALLOC_KIND_MMAP)
        {
            ct_release_memory(ptr, kind);
            return;
        }

        std::free(ptr);
    }

    struct CtLeakReporter
    {
        CT_NOINSTR ~CtLeakReporter()
        {
            std::vector<std::pair<void*, CtAllocEntry>> leaks;

            ct_lock_acquire();
            for (const auto& [ptr, entry] : ct_alloc_table)
            {
                if (entry.state == CT_ENTRY_USED)
                {
                    leaks.push_back({ptr, entry});
                }
            }
            ct_lock_release();

            if (leaks.empty())
            {
                return;
            }

            ct_disable_logging();
            ct_write_prefix(CTLevel::Error);
            ct_write_cstr("ct: leaks detected count=");
            ct_write_dec(leaks.size());
            ct_write_cstr("\n");

            size_t reported = 0;
            for (const auto& [ptr, entry] : leaks)
            {
                ct_write_prefix(CTLevel::Warn);
                ct_write_cstr("ct: leak ptr=");
                ct_write_hex(reinterpret_cast<uintptr_t>(ptr));
                ct_write_cstr(" size=");
                ct_write_dec(entry.size);
                ct_write_cstr(" site=");
                ct_write_cstr(ct_site_name(entry.site));
                ct_write_cstr("\n");

                if (++reported >= 32)
                {
                    ct_write_prefix(CTLevel::Warn);
                    ct_write_cstr("ct: leak list truncated\n");
                    break;
                }
            }
        }
    };

    CtLeakReporter ct_leak_reporter;
} // namespace

CT_NOINSTR void ct_lock_acquire(void)
{
    ct_alloc_mutex.lock();
}

CT_NOINSTR void ct_lock_release(void)
{
    ct_alloc_mutex.unlock();
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
        auto& entry = ct_alloc_table[ptr];
        entry.size = size;
        entry.req_size = req_size;
        entry.site = site;
        entry.state = CT_ENTRY_USED;
        entry.kind = kind;
        return 1;
    }
    catch (...)
    {
        return 0;
    }
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

    auto it = ct_alloc_table.find(const_cast<void*>(ptr));
    if (it == ct_alloc_table.end())
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

CT_NODISCARD CT_NOINSTR int ct_table_lookup_containing(const void* ptr, void** base_out,
                                                       size_t* size_out, size_t* req_size_out,
                                                       const char** site_out,
                                                       unsigned char* state_out)
{
    if (!ptr)
    {
        return 0;
    }

    const uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
    for (const auto& [base_ptr, entry] : ct_alloc_table)
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

extern "C"
{
    CT_NOINSTR void __ct_free(void* ptr);

    CT_NODISCARD CT_NOINSTR void* __ct_malloc(size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return std::malloc(size);
        }
        return ct_record_alloc(std::malloc(size), size, size, site, CT_ALLOC_KIND_MALLOC);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_malloc_unreachable(size_t size, const char* site)
    {
        return __ct_malloc(size, site);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_calloc(size_t count, size_t size, const char* site)
    {
        ct_init_env_once();
        const size_t total = count * size;
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return std::calloc(count, size);
        }
        return ct_record_alloc(std::calloc(count, size), total, total, site, CT_ALLOC_KIND_MALLOC);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_calloc_unreachable(size_t count, size_t size,
                                                          const char* site)
    {
        return __ct_calloc(count, size, site);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new(size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return ::operator new(size);
        }
        return ct_record_alloc(::operator new(size), size, size, site, CT_ALLOC_KIND_NEW);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_unreachable(size_t size, const char* site)
    {
        return __ct_new(size, site);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_array(size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return ::operator new[](size);
        }
        return ct_record_alloc(::operator new[](size), size, size, site, CT_ALLOC_KIND_NEW_ARRAY);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_array_unreachable(size_t size, const char* site)
    {
        return __ct_new_array(size, site);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_nothrow(size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return ::operator new(size, std::nothrow);
        }
        return ct_record_alloc(::operator new(size, std::nothrow), size, size, site,
                               CT_ALLOC_KIND_NEW);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_nothrow_unreachable(size_t size, const char* site)
    {
        return __ct_new_nothrow(size, site);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_array_nothrow(size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return ::operator new[](size, std::nothrow);
        }
        return ct_record_alloc(::operator new[](size, std::nothrow), size, size, site,
                               CT_ALLOC_KIND_NEW_ARRAY);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_new_array_nothrow_unreachable(size_t size, const char* site)
    {
        return __ct_new_array_nothrow(size, site);
    }

    CT_NODISCARD CT_NOINSTR void* __ct_realloc(void* ptr, size_t size, const char* site)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return std::realloc(ptr, size);
        }
        if (!ptr)
        {
            return __ct_malloc(size, site);
        }
        if (size == 0)
        {
            __ct_free(ptr);
            return nullptr;
        }

        void* new_ptr = std::realloc(ptr, size);
        if (!new_ptr)
        {
            return nullptr;
        }

        ct_record_realloc(ptr, new_ptr, size, site);
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
            (void)ct_record_alloc(ptr, size, size, site, CT_ALLOC_KIND_ALIGNED);
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
        return ct_record_alloc(ptr, size, size, site, CT_ALLOC_KIND_ALIGNED);
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

        void* ptr = VirtualAlloc(addr, len, MEM_COMMIT | MEM_RESERVE, ct_translate_page_protection(prot));
        if (!ptr)
        {
            errno = ENOMEM;
            return reinterpret_cast<void*>(-1);
        }

        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            return ptr;
        }
        return ct_record_alloc(ptr, len, len, site, CT_ALLOC_KIND_MMAP);
    }

    CT_NODISCARD CT_NOINSTR int __ct_munmap(void* addr, size_t len, const char* site)
    {
        ct_init_env_once();
        (void)len;

        size_t size = 0;
        const char* alloc_site = nullptr;
        unsigned char kind = CT_ALLOC_KIND_MMAP;

        if (ct_is_enabled(CT_FEATURE_ALLOC))
        {
            ct_lock_acquire();
            (void)ct_remove_for_release(addr, CT_ENTRY_FREED, &size, &alloc_site, &kind);
            ct_lock_release();
            ct_track_shadow_free(addr, size);
        }

        const BOOL ok = addr ? VirtualFree(addr, 0, MEM_RELEASE) : TRUE;
        if (!ok)
        {
            errno = EINVAL;
            return -1;
        }

        ct_log_alloc_event("munmap", addr, size, site ? site : alloc_site, CT_ALLOC_KIND_MMAP);
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
        ct_autofree_impl(ptr, false);
    }

    CT_NOINSTR void __ct_autofree_munmap(void* ptr)
    {
        ct_autofree_impl(ptr, false);
    }

    CT_NOINSTR void __ct_autofree_sbrk(void* ptr)
    {
        ct_autofree_impl(ptr, false);
    }

    CT_NOINSTR void __ct_autofree_delete(void* ptr)
    {
        ct_autofree_impl(ptr, false);
    }

    CT_NOINSTR void __ct_autofree_delete_array(void* ptr)
    {
        ct_autofree_impl(ptr, true);
    }

    CT_NOINSTR void __ct_free(void* ptr)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            std::free(ptr);
            return;
        }

        if (!ptr)
        {
            ct_log_skip_event("free", ptr, "null");
            std::free(ptr);
            return;
        }

        size_t size = 0;
        const char* site = nullptr;
        unsigned char kind = CT_ALLOC_KIND_MALLOC;

        ct_lock_acquire();
        const int found = ct_remove_for_release(ptr, CT_ENTRY_FREED, &size, &site, &kind);
        ct_lock_release();

        if (found == -1)
        {
            ct_log_skip_event("free", ptr, "already freed");
            return;
        }
        if (found == 0)
        {
            ct_log_skip_event("free", ptr, "unknown");
            std::free(ptr);
            return;
        }

        ct_track_shadow_free(ptr, size);
        ct_log_alloc_event("free", ptr, size, site, kind);
        ct_release_memory(ptr, kind);
    }

    CT_NOINSTR void __ct_delete(void* ptr)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            ::operator delete(ptr);
            return;
        }

        size_t size = 0;
        const char* site = nullptr;
        unsigned char kind = CT_ALLOC_KIND_NEW;

        ct_lock_acquire();
        const int found = ct_remove_for_release(ptr, CT_ENTRY_FREED, &size, &site, &kind);
        ct_lock_release();

        if (found == -1)
        {
            ct_log_skip_event("delete", ptr, "already freed");
            return;
        }
        if (found == 0)
        {
            ct_log_skip_event("delete", ptr, "unknown");
            ct_release_unknown_delete(ptr, false);
            return;
        }

        ct_track_shadow_free(ptr, size);
        ct_log_alloc_event("delete", ptr, size, site, kind);
        ct_release_memory(ptr, kind);
    }

    CT_NOINSTR void __ct_delete_array(void* ptr)
    {
        ct_init_env_once();
        if (!ct_is_enabled(CT_FEATURE_ALLOC))
        {
            ::operator delete[](ptr);
            return;
        }

        size_t size = 0;
        const char* site = nullptr;
        unsigned char kind = CT_ALLOC_KIND_NEW_ARRAY;

        ct_lock_acquire();
        const int found = ct_remove_for_release(ptr, CT_ENTRY_FREED, &size, &site, &kind);
        ct_lock_release();

        if (found == -1)
        {
            ct_log_skip_event("delete[]", ptr, "already freed");
            return;
        }
        if (found == 0)
        {
            ct_log_skip_event("delete[]", ptr, "unknown");
            ct_release_unknown_delete(ptr, true);
            return;
        }

        ct_track_shadow_free(ptr, size);
        ct_log_alloc_event("delete[]", ptr, size, site, kind);
        ct_release_memory(ptr, kind);
    }

    CT_NOINSTR void __ct_delete_nothrow(void* ptr)
    {
        __ct_delete(ptr);
    }

    CT_NOINSTR void __ct_delete_array_nothrow(void* ptr)
    {
        __ct_delete_array(ptr);
    }

    CT_NOINSTR void __ct_delete_destroying(void* ptr)
    {
        __ct_delete(ptr);
    }

    CT_NOINSTR void __ct_delete_array_destroying(void* ptr)
    {
        __ct_delete_array(ptr);
    }
}
