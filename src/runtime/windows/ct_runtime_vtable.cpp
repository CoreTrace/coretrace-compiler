// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_vtable_shared.h"

#include "ct_runtime_demangle_cache.h"
#include "ct_runtime_helpers.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <new>
#include <string>
#include <typeinfo>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#pragma comment(lib, "Dbghelp.lib")

namespace
{
    constexpr size_t kBoxMaxValueWidth = 60;

    struct CtRttiCompleteObjectLocator
    {
        std::uint32_t signature;
        std::uint32_t offset;
        std::uint32_t cd_offset;
        std::int32_t type_descriptor_rva;
        std::int32_t class_descriptor_rva;
        std::int32_t self_rva;
    };

    struct CtRttiTypeDescriptor
    {
        const void* vftable;
        void* spare;
        char name[1];
    };

    struct CtVtableInfo
    {
        const void* vtable = nullptr;
        ptrdiff_t offset_to_top = 0;
        std::string dynamic_type = "<unknown>";
        bool has_typeinfo = false;
    };

    struct CtBoxLine
    {
        std::string label;
        std::string value;
    };

    struct CtModuleInfo
    {
        bool resolved = false;
        bool is_main = false;
        bool exec_known = false;
        bool is_exec = false;
        HMODULE handle = nullptr;
        std::string path;
        std::string basename;
    };

    struct CtAddrInfo
    {
        bool has_module = false;
        bool exec_known = false;
        bool is_exec = false;
        bool on_stack = false;
        CtModuleInfo module;
    };

    std::atomic<int> ct_vtable_state_logged{0};

    CT_NODISCARD CT_NOINSTR std::string ct_basename(std::string_view path)
    {
        if (path.empty())
        {
            return {};
        }

        const size_t pos = path.find_last_of("/\\");
        if (pos == std::string_view::npos)
        {
            return std::string(path);
        }
        return std::string(path.substr(pos + 1));
    }

    CT_NODISCARD CT_NOINSTR bool ct_is_executable_protection(DWORD protect)
    {
        const DWORD normalized = protect & 0xFFu;
        return normalized == PAGE_EXECUTE || normalized == PAGE_EXECUTE_READ ||
               normalized == PAGE_EXECUTE_READWRITE || normalized == PAGE_EXECUTE_WRITECOPY;
    }

    CT_NODISCARD CT_NOINSTR bool ct_is_readable(const void* addr, size_t size)
    {
        if (!addr || size == 0)
        {
            return false;
        }

        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0)
        {
            return false;
        }

        if (mbi.State != MEM_COMMIT)
        {
            return false;
        }
        if ((mbi.Protect & PAGE_NOACCESS) != 0 || (mbi.Protect & PAGE_GUARD) != 0)
        {
            return false;
        }

        const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
        const uintptr_t end = start + size;
        const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        return end >= start && end <= region_end;
    }

    // What the loader reports of the modules, where the callback may only record: every load and
    // unload moves `changes`, and an unload also leaves the base of the DLL in `unloaded`, a ring
    // of the last kUnloadedBases. LoadLibrary and FreeLibrary send the notifications with the
    // loader lock held, which makes the callback the only writer; Microsoft does not document
    // it. The TLS callback below starts the watch when the module that holds this copy of the
    // runtime attaches, and stops it when it detaches. The members that are not atomic are used
    // with the DbgHelp lock held. Constant-initialized and trivially destroyed.
    struct CtModuleChanges
    {
        static constexpr unsigned long kUnloadedBases = 64;
        std::atomic<unsigned long> changes{0};
        std::atomic<unsigned long> unloads{0};
        std::atomic<ULONG_PTR> unloaded[kUnloadedBases] = {};
        std::atomic<PVOID> cookie{nullptr};
        // The loader took the callback at attach. Still true once the watch has stopped at
        // detach: the lookups that come after, from the module's destructors, use what it
        // recorded, without listing every module again.
        std::atomic<bool> watched{false};
        unsigned long synced_changes = 0;
        unsigned long synced_unloads = 0;
        bool synced = false;
    };

    // One per module that links the runtime: the executable, and each DLL that links it too.
    constinit CtModuleChanges ct_module_changes;

    // The fields of LDR_DLL_LOADED_NOTIFICATION_DATA, which LDR_DLL_UNLOADED_NOTIFICATION_DATA
    // shares.
    struct CtDllNotificationData
    {
        ULONG flags;
        const void* full_name;
        const void* base_name;
        PVOID base;
        ULONG size;
    };

    CT_NOINSTR VOID CALLBACK ct_module_notification(ULONG reason, const CtDllNotificationData* data,
                                                    PVOID)
    {
        constexpr ULONG kUnloaded = 2; // LDR_DLL_NOTIFICATION_REASON_UNLOADED
        CtModuleChanges& changes = ct_module_changes;
        if (reason == kUnloaded)
        {
            const unsigned long index = changes.unloads.load(std::memory_order_relaxed);
            changes.unloaded[index % CtModuleChanges::kUnloadedBases].store(
                reinterpret_cast<ULONG_PTR>(data->base), std::memory_order_release);
            changes.unloads.store(index + 1, std::memory_order_release);
        }
        changes.changes.fetch_add(1, std::memory_order_release);
    }

    // Asks the loader to report the modules it loads and unloads to ct_module_notification.
    CT_NOINSTR void ct_module_changes_watch()
    {
        using Notification = VOID(CALLBACK*)(ULONG, const CtDllNotificationData*, PVOID);
        using RegisterFn = LONG(NTAPI*)(ULONG, Notification, PVOID, PVOID*);
        auto* register_notification = reinterpret_cast<RegisterFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
        PVOID cookie = nullptr;
        if (register_notification &&
            register_notification(0, &ct_module_notification, nullptr, &cookie) >= 0)
        {
            ct_module_changes.cookie.store(cookie);
            ct_module_changes.watched.store(true);
        }
    }

    CT_NOINSTR BOOL CALLBACK ct_collect_module_base(PCWSTR, DWORD64 base, PVOID bases)
    {
        static_cast<std::vector<DWORD64>*>(bases)->push_back(base);
        return TRUE;
    }

    // DbgHelp keeps the modules it has listed: a DLL unloaded, then replaced by another one at
    // the same base, even at the same path with the same time stamp, kept the first one's names,
    // and a DLL loaded since was unknown (#208). Only the loader tells such loads apart. Once it
    // has loaded or unloaded a module, the session drops the modules at the bases unloaded, then
    // lists the modules loaded now. It drops every module it has instead the first time, for what
    // happened before the watch, without the watch, and when more DLLs were unloaded than the
    // ring keeps. The session itself is kept. The modules count as current only once every step
    // has succeeded: otherwise the function returns false, the lookup names nothing, and the next
    // one starts again from the same point. Call with the DbgHelp lock held.
    CT_NODISCARD CT_NOINSTR bool ct_dbghelp_sync_modules_locked(HANDLE process)
    {
        CtModuleChanges& changes = ct_module_changes;
        const bool watching = changes.watched.load();
        const unsigned long count = changes.changes.load(std::memory_order_acquire);
        if (changes.synced && watching && count == changes.synced_changes)
        {
            return true;
        }

        // The callback writes each base over the one kUnloadedBases unloads older: the bases read
        // are those unloaded only while fewer unloads than that are pending, before and after
        // they are read.
        const unsigned long unloads = changes.unloads.load(std::memory_order_acquire);
        bool recover = !changes.synced || !watching ||
                       unloads - changes.synced_unloads >= CtModuleChanges::kUnloadedBases;
        for (unsigned long index = changes.synced_unloads; !recover && index != unloads; ++index)
        {
            const ULONG_PTR base = changes.unloaded[index % CtModuleChanges::kUnloadedBases].load(
                std::memory_order_acquire);
            if (SymGetModuleBase64(process, base) == base && !SymUnloadModule64(process, base))
            {
                return false;
            }
        }
        recover =
            recover || changes.unloads.load(std::memory_order_acquire) - changes.synced_unloads >=
                           CtModuleChanges::kUnloadedBases;
        if (recover)
        {
            std::vector<DWORD64> bases;
            if (!SymEnumerateModulesW64(process, &ct_collect_module_base, &bases))
            {
                return false;
            }
            for (DWORD64 base : bases)
            {
                if (!SymUnloadModule64(process, base))
                {
                    return false;
                }
            }
        }
        if (!SymRefreshModuleList(process))
        {
            return false;
        }
        changes.synced = true;
        changes.synced_changes = count;
        changes.synced_unloads = unloads;
        return true;
    }

    // Stops the notifications when the module that holds this copy of the runtime detaches: a
    // DLL that FreeLibrary unloads, after which the loader must not call into it, or the
    // executable at exit.
    CT_NOINSTR void ct_module_changes_stop()
    {
        PVOID cookie = ct_module_changes.cookie.exchange(nullptr);
        using UnregisterFn = LONG(NTAPI*)(PVOID);
        auto* unregister_notification = reinterpret_cast<UnregisterFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrUnregisterDllNotification"));
        if (cookie && unregister_notification)
        {
            (void)unregister_notification(cookie);
        }
    }

    // The watch lasts as long as the module that holds this copy of the runtime is attached, from
    // its TLS callbacks: at attach, they run before the module's constructors. No lookup starts
    // the watch, so none starts it again after the stop at detach, which can come before the
    // module's destructors, and their virtual calls.
    CT_NOINSTR void NTAPI ct_module_changes_tls(PVOID, DWORD reason, PVOID)
    {
        if (reason == DLL_PROCESS_ATTACH)
        {
            ct_module_changes_watch();
        }
        else if (reason == DLL_PROCESS_DETACH)
        {
            ct_module_changes_stop();
        }
    }

    CT_NODISCARD CT_NOINSTR bool ct_lookup_symbol_name(const void* addr, std::string& out)
    {
        if (!addr)
        {
            return false;
        }

        char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        // Without DbgHelp or current modules, nothing is named rather than a name that may
        // belong to a DLL unloaded since.
        CtDbgHelpLock dbghelp(CtDbgHelpLock::kLookupWait);
        if (!dbghelp.owns())
        {
            return false;
        }
        ct_dbghelp_initialize_locked();
        HANDLE process = GetCurrentProcess();
        if (!ct_dbghelp_sync_modules_locked(process))
        {
            return false;
        }
        DWORD64 displacement = 0;
        if (SymFromAddr(process, reinterpret_cast<DWORD64>(addr), &displacement, symbol) == FALSE)
        {
            return false;
        }

        out.assign(symbol->Name);
        return true;
    }

    CT_NODISCARD CT_NOINSTR bool ct_address_on_stack(const void* addr)
    {
        if (!addr)
        {
            return false;
        }

        using GetCurrentThreadStackLimitsFn = VOID(WINAPI*)(PULONG_PTR, PULONG_PTR);
        static auto* fn = reinterpret_cast<GetCurrentThreadStackLimitsFn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentThreadStackLimits"));
        if (!fn)
        {
            return false;
        }

        ULONG_PTR low = 0;
        ULONG_PTR high = 0;
        fn(&low, &high);

        const auto value = reinterpret_cast<ULONG_PTR>(addr);
        return value >= low && value < high;
    }

    CT_NODISCARD CT_NOINSTR bool ct_resolve_module(const void* addr, CtModuleInfo& out)
    {
        if (!addr)
        {
            return false;
        }

        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) != 0)
        {
            out.exec_known = true;
            out.is_exec = ct_is_executable_protection(mbi.Protect);
        }

        HMODULE module = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(addr), &module))
        {
            return false;
        }

        char path_buffer[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(module, path_buffer, MAX_PATH);

        out.resolved = true;
        out.handle = module;
        out.is_main = (module == GetModuleHandleA(nullptr));
        if (length != 0)
        {
            out.path.assign(path_buffer, length);
            out.basename = ct_basename(out.path);
        }
        return true;
    }

    CT_NODISCARD CT_NOINSTR CtAddrInfo ct_resolve_address(const void* addr)
    {
        CtAddrInfo info;
        if (!addr)
        {
            return info;
        }

        if (ct_resolve_module(addr, info.module))
        {
            info.has_module = true;
            info.exec_known = info.module.exec_known;
            info.is_exec = info.module.is_exec;
            return info;
        }

        info.on_stack = ct_address_on_stack(addr);
        if (info.on_stack)
        {
            info.exec_known = true;
            info.is_exec = false;
        }
        return info;
    }

    CT_NODISCARD CT_NOINSTR bool ct_modules_match(const CtModuleInfo& lhs, const CtModuleInfo& rhs)
    {
        return lhs.resolved && rhs.resolved && lhs.handle != nullptr && lhs.handle == rhs.handle;
    }

    CT_NODISCARD CT_NOINSTR std::string ct_module_display_name(const CtModuleInfo& info)
    {
        if (!info.resolved)
        {
            return "<unresolved>";
        }
        if (info.is_main)
        {
            return "main";
        }
        if (!info.basename.empty())
        {
            return info.basename;
        }
        if (!info.path.empty())
        {
            return info.path;
        }
        return "<unknown>";
    }

    CT_NODISCARD CT_NOINSTR const CtRttiCompleteObjectLocator*
    ct_get_complete_object_locator(const void* vtable)
    {
        if (!vtable)
        {
            return nullptr;
        }

        auto* table = reinterpret_cast<const void* const*>(vtable);
        const void* locator_ptr = table[-1];
        if (!ct_is_readable(locator_ptr, sizeof(CtRttiCompleteObjectLocator)))
        {
            return nullptr;
        }

        return reinterpret_cast<const CtRttiCompleteObjectLocator*>(locator_ptr);
    }

    CT_NODISCARD CT_NOINSTR const CtRttiTypeDescriptor*
    ct_get_type_descriptor(const CtRttiCompleteObjectLocator* locator)
    {
        if (!locator)
        {
            return nullptr;
        }

        const uintptr_t image_base =
            reinterpret_cast<uintptr_t>(locator) - static_cast<uintptr_t>(locator->self_rva);
        const uintptr_t type_addr =
            image_base + static_cast<uintptr_t>(locator->type_descriptor_rva);
        if (!ct_is_readable(reinterpret_cast<const void*>(type_addr), sizeof(CtRttiTypeDescriptor)))
        {
            return nullptr;
        }

        return reinterpret_cast<const CtRttiTypeDescriptor*>(type_addr);
    }

    // The names of dynamic types and of call targets. Never destroyed: code that runs at exit
    // still prints boxes.
    CT_NODISCARD CT_NOINSTR CtDemangleCache& ct_name_cache()
    {
        alignas(CtDemangleCache) static unsigned char storage[sizeof(CtDemangleCache)];
        static CtDemangleCache* const cache = new (storage) CtDemangleCache(ct_demangle);
        return *cache;
    }

    CT_NODISCARD CT_NOINSTR std::string
    ct_format_type_name(const CtRttiTypeDescriptor* type_descriptor)
    {
        if (!type_descriptor || !ct_is_readable(type_descriptor, sizeof(CtRttiTypeDescriptor)))
        {
            return "<unknown>";
        }

        const char* raw_name = type_descriptor->name;
        if (!raw_name || raw_name[0] == '\0')
        {
            return "<unknown>";
        }

        std::string demangled;
        if (ct_name_cache().demangle(raw_name, demangled))
        {
            return demangled;
        }

        return raw_name;
    }

    CT_NODISCARD CT_NOINSTR bool ct_read_vtable_info(void* this_ptr, CtVtableInfo& info)
    {
        if (!this_ptr)
        {
            return false;
        }

        const void* vtable = *reinterpret_cast<const void* const*>(this_ptr);
        if (!vtable)
        {
            return false;
        }

        info.vtable = vtable;

        const CtRttiCompleteObjectLocator* locator = ct_get_complete_object_locator(vtable);
        if (!locator)
        {
            return true;
        }

        info.offset_to_top = static_cast<ptrdiff_t>(locator->offset);
        info.dynamic_type = ct_format_type_name(ct_get_type_descriptor(locator));
        info.has_typeinfo = info.dynamic_type != "<unknown>";
        return true;
    }

    CT_NOINSTR void ct_append_box_line(std::vector<CtBoxLine>& lines, std::string label,
                                       std::string value)
    {
        lines.push_back({std::move(label), std::move(value)});
    }

    CT_NOINSTR void ct_log_box(CTLevel level, const char* tag, const char* title,
                               const std::vector<CtBoxLine>& lines)
    {
        if (lines.empty())
        {
            return;
        }

        CtLogBlock box;
        size_t label_width = 0;
        size_t value_width = 0;
        for (const auto& line : lines)
        {
            label_width = std::max(label_width, line.label.size());
            value_width = std::max(value_width, std::min(line.value.size(), kBoxMaxValueWidth));
        }
        value_width = std::max<size_t>(value_width, 1);

        const size_t inner_width = label_width + value_width + 5;
        std::string border(inner_width, '-');

        ct_log(level, "[{}]\n", tag ? tag : "BOX");
        ct_log(level, "+{}+\n", border);
        ct_log(level, "| {}{}\n", title ? title : "box",
               std::string(inner_width > ct_strlen(title ? title : "box") + 1
                               ? inner_width - ct_strlen(title ? title : "box") - 1
                               : 0,
                           ' '));
        ct_log(level, "+{}+\n", border);

        for (const auto& line : lines)
        {
            std::string value = line.value.empty() ? "<empty>" : line.value;
            size_t offset = 0;
            bool first = true;
            while (offset < value.size())
            {
                const size_t remaining = value.size() - offset;
                const size_t chunk = std::min(remaining, value_width);
                std::string part = value.substr(offset, chunk);

                std::string label = first ? line.label : std::string();
                if (label.size() < label_width)
                {
                    label.append(label_width - label.size(), ' ');
                }
                if (part.size() < value_width)
                {
                    part.append(value_width - part.size(), ' ');
                }

                ct_log(level, "| {} : {} |\n", label, part);
                offset += chunk;
                first = false;
            }
        }

        ct_log(level, "+{}+\n", border);
    }

    CT_NOINSTR void ct_log_vtable_diag_state(void)
    {
        if (!ct_is_enabled(CT_FEATURE_VTABLE_DIAG))
        {
            return;
        }

        int expected = 0;
        if (!ct_vtable_state_logged.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        {
            return;
        }

        if (ct_is_enabled(CT_FEATURE_ALLOC))
        {
            ct_log(CTLevel::Info, "[VTABLE-DIAG]: alloc-tracking=enabled\n");
            return;
        }

        std::string reason = "unknown";
        if (ct_alloc_disabled_by_env)
        {
            reason = "env CT_DISABLE_ALLOC";
        }
        else if (ct_alloc_disabled_by_config)
        {
            reason = "compile-time --ct-no-alloc/--ct-modules";
        }
        ct_log(CTLevel::Info, "[VTABLE-DIAG]: alloc-tracking=disabled (reason={})\n", reason);
    }
} // namespace

// True when alloc tracking knows this object was already released. Checked before
// any read through the pointer: a freed object's vptr is garbage and following it
// would crash the diagnostic that is supposed to report the use-after-free.
extern "C"
{
    CT_NOINSTR void __ct_vtable_dump(void* this_ptr, const char* site, const char* static_type)
    {
        CtRuntimeScope scope;
        ct_init_env_once();
        ct_log_vtable_diag_state();

        CtVtableInfo info;
        const bool is_freed = ct_object_is_freed(this_ptr);
        const bool has_vtable = !is_freed && ct_read_vtable_info(this_ptr, info);
        const char* site_name = ct_site_name(site);

        std::vector<CtBoxLine> lines;
        ct_append_box_line(lines, "site", site_name ? site_name : "<unknown>");
        ct_append_box_line(lines, "this",
                           this_ptr ? std::format("{:p}", this_ptr) : std::string("<null>"));
        ct_append_box_line(lines, "vtable",
                           (has_vtable && info.vtable) ? std::format("{:p}", info.vtable)
                                                       : std::string("<null>"));
        if (has_vtable)
        {
            ct_append_box_line(lines, "off_top", std::to_string(info.offset_to_top));
        }
        ct_append_box_line(lines, "type", info.dynamic_type);
        if (ct_is_enabled(CT_FEATURE_VTABLE_DIAG) && !ct_is_unknown_type(static_type))
        {
            ct_append_box_line(lines, "static", static_type);
        }

        std::vector<std::string> warnings;
        // Warnings are diagnostics: the POSIX port only emits them under
        // --ct-vtable-diag and this port must not be noisier.
        if (ct_is_enabled(CT_FEATURE_VTABLE_DIAG))
        {
            if (!this_ptr)
            {
                warnings.push_back("null this pointer");
            }
            if (!has_vtable && !is_freed)
            {
                warnings.push_back("no vptr");
            }
            if (has_vtable && !info.has_typeinfo)
            {
                warnings.push_back("missing RTTI");
            }

            if (has_vtable)
            {
                const CtAddrInfo vtable_addr = ct_resolve_address(info.vtable);
                if (vtable_addr.has_module)
                {
                    ct_append_box_line(lines, "vmod", ct_module_display_name(vtable_addr.module));
                }
                else
                {
                    warnings.push_back("vtable resolve failed");
                }
            }

            if (is_freed)
            {
                warnings.push_back("vptr on freed object");
            }

            if (!ct_is_unknown_type(static_type) && info.dynamic_type != "<unknown>" &&
                info.dynamic_type != static_type)
            {
                warnings.push_back("static!=dynamic type");
            }
        }

        for (const auto& warning : warnings)
        {
            ct_append_box_line(lines, "warn", warning);
        }

        ct_log_box(warnings.empty() ? CTLevel::Info : CTLevel::Warn, "VTABLE", "vtable", lines);
    }

    CT_NOINSTR void __ct_vcall_trace(void* this_ptr, void* target, const char* site,
                                     const char* static_type)
    {
        CtRuntimeScope scope;
        ct_init_env_once();
        ct_log_vtable_diag_state();

        CtVtableInfo info;
        const bool is_freed = ct_object_is_freed(this_ptr);
        const bool has_vtable = !is_freed && ct_read_vtable_info(this_ptr, info);
        const char* site_name = ct_site_name(site);

        std::string symbol_name = "<unknown>";
        std::string demangled_name = "<unknown>";
        if (target && ct_lookup_symbol_name(target, symbol_name))
        {
            std::string pretty;
            if (ct_name_cache().demangle(symbol_name.c_str(), pretty))
            {
                demangled_name = pretty;
            }
            else
            {
                demangled_name = symbol_name;
            }
        }

        std::vector<CtBoxLine> lines;
        ct_append_box_line(lines, "site", site_name ? site_name : "<unknown>");
        ct_append_box_line(lines, "this",
                           this_ptr ? std::format("{:p}", this_ptr) : std::string("<null>"));
        ct_append_box_line(lines, "vtable",
                           (has_vtable && info.vtable) ? std::format("{:p}", info.vtable)
                                                       : std::string("<null>"));
        ct_append_box_line(lines, "type", info.dynamic_type);
        ct_append_box_line(lines, "target",
                           target ? std::format("{:p}", target) : std::string("<null>"));
        ct_append_box_line(lines, "symbol", symbol_name);
        ct_append_box_line(lines, "demangled", demangled_name);
        if (ct_is_enabled(CT_FEATURE_VTABLE_DIAG) && !ct_is_unknown_type(static_type))
        {
            ct_append_box_line(lines, "static", static_type);
        }

        std::vector<std::string> warnings;
        // Warnings are diagnostics: the POSIX port only emits them under
        // --ct-vtable-diag and this port must not be noisier.
        if (ct_is_enabled(CT_FEATURE_VTABLE_DIAG))
        {
            if (!this_ptr)
            {
                warnings.push_back("null this pointer");
            }
            if (!has_vtable && !is_freed)
            {
                warnings.push_back("no vptr");
            }
            if (has_vtable && !info.has_typeinfo)
            {
                warnings.push_back("missing RTTI");
            }

            const CtAddrInfo vtable_addr =
                has_vtable ? ct_resolve_address(info.vtable) : CtAddrInfo{};
            const CtAddrInfo target_addr = target ? ct_resolve_address(target) : CtAddrInfo{};

            if (vtable_addr.has_module)
            {
                ct_append_box_line(lines, "vmod", ct_module_display_name(vtable_addr.module));
            }
            else if (has_vtable)
            {
                warnings.push_back("vtable resolve failed");
            }

            if (target_addr.has_module)
            {
                ct_append_box_line(lines, "tmod", ct_module_display_name(target_addr.module));
            }
            else if (target_addr.exec_known && !target_addr.is_exec)
            {
                warnings.push_back("target in non-exec memory");
            }
            else if (target_addr.on_stack)
            {
                warnings.push_back("target points to stack memory");
            }

            if (is_freed)
            {
                warnings.push_back("vptr on freed object");
            }

            if (!ct_is_unknown_type(static_type) && info.dynamic_type != "<unknown>" &&
                info.dynamic_type != static_type)
            {
                warnings.push_back("static!=dynamic type");
            }

            if (vtable_addr.has_module && target_addr.has_module &&
                !ct_modules_match(vtable_addr.module, target_addr.module))
            {
                warnings.push_back("module mismatch between vtable and target");
            }
        }

        for (const auto& warning : warnings)
        {
            ct_append_box_line(lines, "warn", warning);
        }

        ct_log_box(warnings.empty() ? CTLevel::Info : CTLevel::Warn, "VCALL", "vcall", lines);
    }
}

// The TLS callback of the watch, in the file of the lookups that read it, so that a module
// linking one links the other.
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:ct_module_changes_tls_callback")
#pragma section(".CRT$XLY", read)
extern "C" __declspec(allocate(".CRT$XLY")) __attribute__((used))
const PIMAGE_TLS_CALLBACK ct_module_changes_tls_callback = ct_module_changes_tls;
