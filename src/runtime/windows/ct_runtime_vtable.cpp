// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_internal.h"

#include "ct_runtime_helpers.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
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

    std::once_flag ct_symbols_once;
    std::atomic<int> ct_vtable_state_logged{0};

    CT_NOINSTR void ct_ensure_symbols(void)
    {
        std::call_once(ct_symbols_once,
                       []
                       {
                           HANDLE process = GetCurrentProcess();
                           SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES |
                                         SYMOPT_UNDNAME);
                           (void)SymInitialize(process, nullptr, TRUE);
                       });
    }

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

    CT_NODISCARD CT_NOINSTR bool ct_lookup_symbol_name(const void* addr, std::string& out)
    {
        if (!addr)
        {
            return false;
        }

        ct_ensure_symbols();

        char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        DWORD64 displacement = 0;
        if (SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(addr), &displacement,
                        symbol) == FALSE)
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
        if (ct_demangle(raw_name, demangled))
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

    CT_NODISCARD CT_NOINSTR bool ct_is_unknown_type(const char* type_name)
    {
        return !type_name || type_name[0] == '\0' || ct_streq(type_name, "<unknown>");
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

extern "C"
{
    CT_NOINSTR void __ct_vtable_dump(void* this_ptr, const char* site, const char* static_type)
    {
        ct_init_env_once();
        if (!ct_log_is_enabled())
        {
            ct_enable_logging();
            ct_maybe_install_backtrace();
        }
        ct_log_vtable_diag_state();

        CtVtableInfo info;
        const bool has_vtable = ct_read_vtable_info(this_ptr, info);
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
        if (!this_ptr)
        {
            warnings.push_back("null this pointer");
        }
        if (!has_vtable)
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

        if (ct_is_enabled(CT_FEATURE_ALLOC))
        {
            unsigned char state = 0;
            ct_lock_acquire();
            const int found =
                ct_table_lookup_containing(this_ptr, nullptr, nullptr, nullptr, nullptr, &state);
            ct_lock_release();
            if (found && state == CT_ENTRY_FREED)
            {
                warnings.push_back("vptr on freed object");
            }
        }

        if (!ct_is_unknown_type(static_type) && info.dynamic_type != "<unknown>" &&
            info.dynamic_type != static_type)
        {
            warnings.push_back("static!=dynamic type");
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
        ct_init_env_once();
        if (!ct_log_is_enabled())
        {
            ct_enable_logging();
            ct_maybe_install_backtrace();
        }
        ct_log_vtable_diag_state();

        CtVtableInfo info;
        const bool has_vtable = ct_read_vtable_info(this_ptr, info);
        const char* site_name = ct_site_name(site);

        std::string symbol_name = "<unknown>";
        std::string demangled_name = "<unknown>";
        if (target && ct_lookup_symbol_name(target, symbol_name))
        {
            std::string pretty;
            if (ct_demangle(symbol_name.c_str(), pretty))
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
        if (!this_ptr)
        {
            warnings.push_back("null this pointer");
        }
        if (!has_vtable)
        {
            warnings.push_back("no vptr");
        }
        if (has_vtable && !info.has_typeinfo)
        {
            warnings.push_back("missing RTTI");
        }

        const CtAddrInfo vtable_addr = has_vtable ? ct_resolve_address(info.vtable) : CtAddrInfo{};
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

        if (ct_is_enabled(CT_FEATURE_ALLOC))
        {
            unsigned char state = 0;
            ct_lock_acquire();
            const int found =
                ct_table_lookup_containing(this_ptr, nullptr, nullptr, nullptr, nullptr, &state);
            ct_lock_release();
            if (found && state == CT_ENTRY_FREED)
            {
                warnings.push_back("vptr on freed object");
            }
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

        for (const auto& warning : warnings)
        {
            ct_append_box_line(lines, "warn", warning);
        }

        ct_log_box(warnings.empty() ? CTLevel::Info : CTLevel::Warn, "VCALL", "vcall", lines);
    }
}
