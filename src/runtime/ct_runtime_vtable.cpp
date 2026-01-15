#include "ct_runtime_internal.h"

#include "ct_runtime_helpers.h"

#include <cstddef>
#include <cstdlib>
#include <stdlib.h>
#include <limits.h>
#include <typeinfo>
#include <vector>
#include <pthread.h>
#if defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#endif
#if defined(__linux__)
#include <link.h>
#include <elf.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/vm_prot.h>
#endif

namespace {

constexpr size_t kBoxMaxValueWidth = 40;

struct CtVtableInfo {
    const void *vtable = nullptr;
    const std::type_info *typeinfo = nullptr;
    ptrdiff_t offset_to_top = 0;
};

struct CtBoxLine {
    std::string label;
    std::string value;
};

struct CtModuleInfo {
    bool resolved = false;
    bool is_main = false;
    bool exec_known = false;
    bool is_exec = false;
    std::string path;
    std::string realpath;
    std::string basename;
    std::string install_name;
    std::string framework;
};

struct CtAddrInfo {
    bool has_module = false;
    bool exec_known = false;
    bool is_exec = false;
    bool on_stack = false;
    CtModuleInfo module;
};

CT_NOINSTR bool ct_demangle_any(const char *name, std::string &out)
{
    if (!name) {
        return false;
    }

    int status = 0;
    size_t length = 0;
    char *demangled = abi::__cxa_demangle(name, nullptr, &length, &status);
    if (status == 0 && demangled) {
        out.assign(demangled);
        std::free(demangled);
        return true;
    }
    if (demangled) {
        std::free(demangled);
    }
    return false;
}

CT_NOINSTR bool ct_lookup_symbol(const void *addr, const char **name_out, const char **object_out)
{
#if defined(__APPLE__) || defined(__linux__)
    Dl_info info;
    if (dladdr(addr, &info) == 0) {
        return false;
    }
    if (name_out) {
        *name_out = info.dli_sname;
    }
    if (object_out) {
        *object_out = info.dli_fname;
    }
    return info.dli_sname != nullptr || info.dli_fname != nullptr;
#else
    (void)addr;
    (void)name_out;
    (void)object_out;
    return false;
#endif
}

CT_NOINSTR bool ct_read_vtable_info(void *this_ptr, CtVtableInfo &info)
{
    if (!this_ptr) {
        return false;
    }

    const void *vtable = *reinterpret_cast<const void *const *>(this_ptr);
    if (!vtable) {
        return false;
    }

    const void *const *vtable_ptr = reinterpret_cast<const void *const *>(vtable);
    info.vtable = vtable;
    info.typeinfo = reinterpret_cast<const std::type_info *>(vtable_ptr[-1]);
    info.offset_to_top = *reinterpret_cast<const ptrdiff_t *>(vtable_ptr - 2);
    return true;
}

CT_NOINSTR std::string ct_format_type_name(const std::type_info *typeinfo)
{
    if (!typeinfo) {
        return "<unknown>";
    }

    std::string demangled;
    const char *name = typeinfo->name();
    if (ct_demangle_any(name, demangled)) {
        return demangled;
    }
    return name ? std::string(name) : "<unknown>";
}

CT_NOINSTR std::string ct_basename(const std::string &path)
{
    if (path.empty()) {
        return {};
    }
    size_t end = path.size();
    while (end > 0 && path[end - 1] == '/') {
        --end;
    }
    if (end == 0) {
        return {};
    }
    size_t pos = path.rfind('/', end - 1);
    if (pos == std::string::npos) {
        return path.substr(0, end);
    }
    return path.substr(pos + 1, end - pos - 1);
}

CT_NOINSTR std::string ct_make_realpath(const std::string &path)
{
    if (path.empty()) {
        return {};
    }
    char resolved[PATH_MAX];
    if (!realpath(path.c_str(), resolved)) {
        return {};
    }
    return std::string(resolved);
}

CT_NOINSTR std::string ct_framework_name(const std::string &path)
{
#if defined(__APPLE__)
    const std::string marker = ".framework/";
    size_t pos = path.rfind(marker);
    if (pos == std::string::npos) {
        return {};
    }
    size_t start = path.rfind('/', pos);
    if (start == std::string::npos) {
        start = 0;
    } else {
        start += 1;
    }
    if (pos <= start) {
        return {};
    }
    return path.substr(start, pos - start);
#else
    (void)path;
    return {};
#endif
}

CT_NOINSTR const std::string &ct_executable_path(void)
{
    static std::string cached;
    static int initialized = 0;
    if (initialized) {
        return cached;
    }
    initialized = 1;
#if defined(__APPLE__)
    uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    if (size > 0) {
        std::string buffer(size, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
            buffer.resize(ct_strlen(buffer.c_str()));
            cached = buffer;
        }
    }
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        cached.assign(buf);
    }
#endif
    return cached;
}

CT_NOINSTR void ct_fill_module_paths(CtModuleInfo &info, const std::string &path)
{
    info.path = path;
    info.realpath = ct_make_realpath(path);
    info.basename = ct_basename(path);
    info.framework = ct_framework_name(path);
}

CT_NOINSTR bool ct_address_on_stack(const void *addr)
{
    if (!addr) {
        return false;
    }
#if defined(__APPLE__)
    pthread_t thread = pthread_self();
    void *stack_end = pthread_get_stackaddr_np(thread);
    size_t stack_size = pthread_get_stacksize_np(thread);
    uintptr_t end = reinterpret_cast<uintptr_t>(stack_end);
    uintptr_t start = end - stack_size;
    uintptr_t value = reinterpret_cast<uintptr_t>(addr);
    return value >= start && value < end;
#elif defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) {
        return false;
    }
    void *stack_base = nullptr;
    size_t stack_size = 0;
    int rc = pthread_attr_getstack(&attr, &stack_base, &stack_size);
    pthread_attr_destroy(&attr);
    if (rc != 0 || !stack_base || stack_size == 0) {
        return false;
    }
    uintptr_t start = reinterpret_cast<uintptr_t>(stack_base);
    uintptr_t end = start + stack_size;
    uintptr_t value = reinterpret_cast<uintptr_t>(addr);
    return value >= start && value < end;
#else
    (void)addr;
    return false;
#endif
}

CT_NOINSTR bool ct_modules_match(const CtModuleInfo &lhs, const CtModuleInfo &rhs)
{
    if (!lhs.resolved || !rhs.resolved) {
        return false;
    }
    if (lhs.is_main && rhs.is_main) {
        return true;
    }
    auto match = [](const std::string &a, const std::string &b) {
        return !a.empty() && !b.empty() && a == b;
    };

    if (match(lhs.realpath, rhs.realpath) ||
        match(lhs.path, rhs.path) ||
        match(lhs.basename, rhs.basename) ||
        match(lhs.install_name, rhs.install_name) ||
        match(lhs.framework, rhs.framework)) {
        return true;
    }

    if (match(lhs.install_name, rhs.path) ||
        match(lhs.install_name, rhs.basename) ||
        match(rhs.install_name, lhs.path) ||
        match(rhs.install_name, lhs.basename)) {
        return true;
    }

    return false;
}

CT_NOINSTR std::string ct_module_display_name(const CtModuleInfo &info)
{
    if (!info.resolved) {
        return "<unresolved>";
    }
    if (info.is_main) {
        return "main";
    }
    if (!info.basename.empty()) {
        return info.basename;
    }
    if (!info.install_name.empty()) {
        return info.install_name;
    }
    if (!info.path.empty()) {
        return info.path;
    }
    return "<unknown>";
}

#if defined(__linux__)
struct CtLinuxFindContext {
    const void *addr = nullptr;
    CtModuleInfo *out = nullptr;
    bool found = false;
};

CT_NOINSTR static int ct_linux_phdr_cb(struct dl_phdr_info *info, size_t, void *data)
{
    auto *ctx = static_cast<CtLinuxFindContext *>(data);
    if (!ctx || !ctx->addr || !ctx->out) {
        return 0;
    }

    uintptr_t addr = reinterpret_cast<uintptr_t>(ctx->addr);
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto &phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD) {
            continue;
        }
        uintptr_t seg_start = static_cast<uintptr_t>(info->dlpi_addr) + phdr.p_vaddr;
        uintptr_t seg_end = seg_start + phdr.p_memsz;
        if (addr < seg_start || addr >= seg_end) {
            continue;
        }

        std::string path = (info->dlpi_name && info->dlpi_name[0] != '\0')
                               ? std::string(info->dlpi_name)
                               : ct_executable_path();
        ctx->out->resolved = true;
        ctx->out->is_main = (info->dlpi_name == nullptr || info->dlpi_name[0] == '\0');
        ctx->out->exec_known = true;
        ctx->out->is_exec = (phdr.p_flags & PF_X) != 0;
        ct_fill_module_paths(*ctx->out, path);
        ctx->found = true;
        return 1;
    }
    return 0;
}
#endif

#if defined(__APPLE__)
CT_NOINSTR bool ct_find_module_macos(const void *addr, CtModuleInfo &out)
{
    if (!addr) {
        return false;
    }

    uintptr_t value = reinterpret_cast<uintptr_t>(addr);
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        const struct mach_header *header = _dyld_get_image_header(i);
        if (!header) {
            continue;
        }
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        bool is64 = (header->magic == MH_MAGIC_64 || header->magic == MH_CIGAM_64);
        const uint8_t *cmd_ptr = reinterpret_cast<const uint8_t *>(header) +
                                 (is64 ? sizeof(struct mach_header_64) : sizeof(struct mach_header));
        bool matched = false;
        bool exec_flag = false;
        std::string install_name;

        for (uint32_t cmd_index = 0; cmd_index < header->ncmds; ++cmd_index) {
            const auto *cmd = reinterpret_cast<const struct load_command *>(cmd_ptr);
            if (cmd->cmd == LC_ID_DYLIB && install_name.empty()) {
                const auto *dylib_cmd = reinterpret_cast<const struct dylib_command *>(cmd);
                const char *name = reinterpret_cast<const char *>(cmd) + dylib_cmd->dylib.name.offset;
                if (name && name[0] != '\0') {
                    install_name = std::string(name);
                }
            }

            if (cmd->cmd == LC_SEGMENT_64 && is64) {
                const auto *seg = reinterpret_cast<const struct segment_command_64 *>(cmd);
                uintptr_t start = static_cast<uintptr_t>(seg->vmaddr) + static_cast<uintptr_t>(slide);
                uintptr_t end = start + static_cast<uintptr_t>(seg->vmsize);
                if (value >= start && value < end) {
                    matched = true;
                    exec_flag = (seg->initprot & VM_PROT_EXECUTE) != 0;
                }
            } else if (cmd->cmd == LC_SEGMENT && !is64) {
                const auto *seg = reinterpret_cast<const struct segment_command *>(cmd);
                uintptr_t start = static_cast<uintptr_t>(seg->vmaddr) + static_cast<uintptr_t>(slide);
                uintptr_t end = start + static_cast<uintptr_t>(seg->vmsize);
                if (value >= start && value < end) {
                    matched = true;
                    exec_flag = (seg->initprot & VM_PROT_EXECUTE) != 0;
                }
            }

            cmd_ptr += cmd->cmdsize;
        }

        if (!matched) {
            continue;
        }

        const char *path_cstr = _dyld_get_image_name(i);
        std::string path = (path_cstr && path_cstr[0] != '\0') ? std::string(path_cstr)
                                                               : ct_executable_path();
        out.resolved = true;
        out.is_main = (i == 0);
        out.exec_known = true;
        out.is_exec = exec_flag;
        out.install_name = install_name;
        ct_fill_module_paths(out, path);
        return true;
    }
    return false;
}
#endif

CT_NOINSTR bool ct_resolve_module(const void *addr, CtModuleInfo &out)
{
    if (!addr) {
        return false;
    }

#if defined(__linux__)
    CtLinuxFindContext ctx;
    ctx.addr = addr;
    ctx.out = &out;
    dl_iterate_phdr(ct_linux_phdr_cb, &ctx);
    if (ctx.found) {
        return true;
    }
#elif defined(__APPLE__)
    if (ct_find_module_macos(addr, out)) {
        return true;
    }
#endif

#if defined(__APPLE__) || defined(__linux__)
    Dl_info info;
    if (dladdr(addr, &info) != 0 && info.dli_fname) {
        out.resolved = true;
        out.exec_known = false;
        out.is_exec = false;
        ct_fill_module_paths(out, info.dli_fname);
        out.is_main = false;
        std::string exe = ct_executable_path();
        if (!exe.empty()) {
            std::string exe_real = ct_make_realpath(exe);
            if (out.path == exe || (!exe_real.empty() && out.realpath == exe_real)) {
                out.is_main = true;
            }
        }
        return true;
    }
#endif

    return false;
}

CT_NOINSTR CtAddrInfo ct_resolve_address(const void *addr)
{
    CtAddrInfo info;
    if (!addr) {
        return info;
    }

    if (ct_resolve_module(addr, info.module)) {
        info.has_module = true;
        info.exec_known = info.module.exec_known;
        info.is_exec = info.module.is_exec;
        return info;
    }

    if (ct_address_on_stack(addr)) {
        info.exec_known = true;
        info.is_exec = false;
        info.on_stack = true;
    }
    return info;
}

CT_NOINSTR void ct_append_repeat(std::string &out, const char *token, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        out.append(token);
    }
}

CT_NOINSTR void ct_log_box(CTLevel level,
                           const char *tag,
                           const char *title,
                           const CtBoxLine *lines,
                           size_t count)
{
    if (!lines || count == 0) {
        return;
    }

    const char *safe_tag = tag ? tag : "BOX";
    const char *safe_title = title ? title : safe_tag;

    size_t label_width = 0;
    size_t value_width = 0;
    for (size_t i = 0; i < count; ++i) {
        if (lines[i].label.size() > label_width) {
            label_width = lines[i].label.size();
        }
        if (lines[i].value.size() > value_width) {
            value_width = lines[i].value.size();
        }
    }
    if (value_width > kBoxMaxValueWidth) {
        value_width = kBoxMaxValueWidth;
    }
    if (value_width == 0) {
        value_width = 1;
    }

    size_t inner_width = label_width + value_width + 5;
    size_t title_len = ct_strlen(safe_title);
    size_t dash_count = 1;
    if (inner_width > title_len + 3) {
        dash_count = inner_width - title_len - 3;
    }

    ct_log(level, "[{}]\n", safe_tag);

    std::string top;
    top.reserve(inner_width + 8);
    top.append("┌─ ");
    top.append(safe_title);
    top.push_back(' ');
    ct_append_repeat(top, "─", dash_count);
    top.append("┐\n");
    ct_log(level, "{}", top);

    for (size_t i = 0; i < count; ++i) {
        std::string_view label = lines[i].label;
        std::string_view value = lines[i].value;
        size_t offset = 0;
        bool first = true;
        if (value.empty()) {
            value = "<empty>";
        }
        while (offset < value.size()) {
            size_t chunk = value.size() - offset;
            if (chunk > value_width) {
                chunk = value_width;
            }
            std::string_view part = value.substr(offset, chunk);

            std::string row;
            row.reserve(inner_width + 8);
            row.append("│ ");
            if (first) {
                row.append(label);
                if (label.size() < label_width) {
                    row.append(label_width - label.size(), ' ');
                }
            } else {
                row.append(label_width, ' ');
            }
            row.append(" : ");
            row.append(part);
            if (part.size() < value_width) {
                row.append(value_width - part.size(), ' ');
            }
            row.append(" │\n");
            ct_log(level, "{}", row);

            offset += chunk;
            first = false;
        }
    }

    std::string bottom;
    bottom.reserve(inner_width + 8);
    bottom.append("└");
    ct_append_repeat(bottom, "─", inner_width);
    bottom.append("┘\n");
    ct_log(level, "{}", bottom);
}

CT_NOINSTR bool ct_is_unknown_type(const char *type_name)
{
    if (!type_name || type_name[0] == '\0') {
        return true;
    }
    return ct_streq(type_name, "<unknown>") != 0;
}

CT_NOINSTR void ct_log_vtable_diag_state(void)
{
    static int logged = 0;
    if (!ct_vtable_diag_enabled) {
        return;
    }
    int expected = 0;
    if (!__atomic_compare_exchange_n(&logged,
                                     &expected,
                                     1,
                                     false,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        return;
    }

    if (!ct_disable_alloc) {
        ct_log(CTLevel::Info, "[VTABLE-DIAG]: alloc-tracking=enabled\n");
        return;
    }

    std::string reason = "unknown";
    if (ct_alloc_disabled_by_env) {
        reason = "env CT_DISABLE_ALLOC";
    } else if (ct_alloc_disabled_by_config) {
        reason = "compile-time --ct-no-alloc/--ct-modules";
    }
    ct_log(CTLevel::Info,
           "[VTABLE-DIAG]: alloc-tracking=disabled (reason={})\n",
           reason);
}

} // namespace

extern "C" {

CT_NOINSTR void __ct_vtable_dump(void *this_ptr, const char *site, const char *static_type)
{
    ct_init_env_once();
    if (!ct_log_is_enabled()) {
        ct_enable_logging();
        ct_maybe_install_backtrace();
    }
    ct_log_vtable_diag_state();

    const char *site_name = ct_site_name(site);
    CtVtableInfo info;
    bool has_vtable = ct_read_vtable_info(this_ptr, info);
    std::string type_name = has_vtable ? ct_format_type_name(info.typeinfo) : "<unknown>";
    std::string this_value = this_ptr ? std::format("{:p}", this_ptr) : "<null>";
    std::string vtable_value = has_vtable && info.vtable ? std::format("{:p}", info.vtable) : "<null>";

    std::vector<CtBoxLine> lines;
    lines.push_back({"site", site_name ? site_name : "<unknown>"});
    lines.push_back({"this", this_value});
    if (has_vtable) {
        lines.push_back({"vtable", vtable_value});
        lines.push_back({"off_top", std::to_string(info.offset_to_top)});
    }
    lines.push_back({"type", type_name});

    if (ct_vtable_diag_enabled && !ct_is_unknown_type(static_type)) {
        lines.push_back({"static", static_type});
    }

    std::vector<std::string> warnings;
    if (ct_vtable_diag_enabled) {
        if (!this_ptr) {
            warnings.push_back("null this pointer");
        }
        if (!has_vtable) {
            warnings.push_back("no vptr");
        }
        if (has_vtable && !info.typeinfo) {
            warnings.push_back("missing typeinfo");
        }

        if (has_vtable) {
            CtAddrInfo vtable_addr = ct_resolve_address(info.vtable);
            if (vtable_addr.has_module) {
                lines.push_back({"vmod", ct_module_display_name(vtable_addr.module)});
            } else {
                warnings.push_back("vtable resolve failed");
            }
        }

        if (!ct_disable_alloc) {
            unsigned char state = 0;
            if (ct_table_lookup_containing(this_ptr, nullptr, nullptr, nullptr, nullptr, &state) &&
                state == CT_ENTRY_FREED) {
                warnings.push_back("vptr on freed object");
            }
        }

        if (!ct_is_unknown_type(static_type) && type_name != "<unknown>" &&
            type_name != static_type) {
            warnings.push_back("static!=dynamic type");
        }
    }

    for (const auto &warn : warnings) {
        lines.push_back({"warn", warn});
    }

    CTLevel level = warnings.empty() ? CTLevel::Info : CTLevel::Warn;
    ct_log_box(level, "VTABLE", "vtable", lines.data(), lines.size());
}

CT_NOINSTR void __ct_vcall_trace(void *this_ptr,
                                 void *target,
                                 const char *site,
                                 const char *static_type)
{
    ct_init_env_once();
    if (!ct_log_is_enabled()) {
        ct_enable_logging();
        ct_maybe_install_backtrace();
    }
    ct_log_vtable_diag_state();

    const char *site_name = ct_site_name(site);
    CtVtableInfo info;
    bool has_vtable = ct_read_vtable_info(this_ptr, info);
    std::string type_name = has_vtable ? ct_format_type_name(info.typeinfo) : "<unknown>";

    const char *sym = nullptr;
    std::string demangled;
    if (target && ct_lookup_symbol(target, &sym, nullptr) && sym) {
        ct_demangle_any(sym, demangled);
    }

    std::string this_value = this_ptr ? std::format("{:p}", this_ptr) : "<null>";
    std::string vtable_value = has_vtable && info.vtable ? std::format("{:p}", info.vtable) : "<unknown>";
    std::string target_value = target ? std::format("{:p}", target) : "<null>";
    std::string sym_name = sym ? std::string(sym) : "<unknown>";
    std::string demangled_name = !demangled.empty() ? demangled : "<unknown>";

    std::vector<CtBoxLine> lines;
    lines.push_back({"site", site_name ? site_name : "<unknown>"});
    lines.push_back({"this", this_value});
    lines.push_back({"vtable", vtable_value});
    lines.push_back({"type", type_name});
    lines.push_back({"target", target_value});
    lines.push_back({"symbol", sym_name});
    lines.push_back({"demangled", demangled_name});
    if (ct_vtable_diag_enabled && !ct_is_unknown_type(static_type)) {
        lines.push_back({"static", static_type});
    }

    std::vector<std::string> warnings;
    CtAddrInfo vtable_addr;
    CtAddrInfo target_addr;
    if (ct_vtable_diag_enabled) {
        if (!this_ptr) {
            warnings.push_back("null this pointer");
        }
        if (!has_vtable) {
            warnings.push_back("no vptr");
        }
        if (has_vtable && !info.typeinfo) {
            warnings.push_back("missing typeinfo");
        }

        if (has_vtable) {
            vtable_addr = ct_resolve_address(info.vtable);
            if (vtable_addr.has_module) {
                lines.push_back({"vmod", ct_module_display_name(vtable_addr.module)});
            } else {
                warnings.push_back("vtable resolve failed");
            }
        }

        if (target) {
            target_addr = ct_resolve_address(target);
            if (target_addr.has_module) {
                lines.push_back({"tmod", ct_module_display_name(target_addr.module)});
            }
        }

        if (!ct_disable_alloc) {
            unsigned char state = 0;
            if (ct_table_lookup_containing(this_ptr, nullptr, nullptr, nullptr, nullptr, &state) &&
                state == CT_ENTRY_FREED) {
                warnings.push_back("vptr on freed object");
            }
        }

        if (!ct_is_unknown_type(static_type) && type_name != "<unknown>" &&
            type_name != static_type) {
            warnings.push_back("static!=dynamic type");
        }

        if (vtable_addr.has_module && target_addr.has_module) {
            if (!ct_modules_match(vtable_addr.module, target_addr.module)) {
                std::string warn = "module mismatch: vtable=";
                warn += ct_module_display_name(vtable_addr.module);
                warn += " target=";
                warn += ct_module_display_name(target_addr.module);
                warnings.push_back(warn);
            }
        } else if (vtable_addr.has_module && !target_addr.has_module && target) {
            if (target_addr.exec_known && !target_addr.is_exec) {
                warnings.push_back("target in non-exec memory");
            } else {
                lines.push_back({"note", "target module unresolved"});
            }
        } else if (!vtable_addr.has_module && target_addr.has_module) {
            lines.push_back({"note", "vtable module unresolved"});
        } else if (!vtable_addr.has_module && !target_addr.has_module && target) {
            if (target_addr.exec_known && !target_addr.is_exec) {
                warnings.push_back("target in non-exec memory");
            } else {
                lines.push_back({"note", "modules unresolved"});
            }
        }
    }

    for (const auto &warn : warnings) {
        lines.push_back({"warn", warn});
    }

    CTLevel level = warnings.empty() ? CTLevel::Info : CTLevel::Warn;
    ct_log_box(level, "VCALL", "vcall", lines.data(), lines.size());
}

} // extern "C"
