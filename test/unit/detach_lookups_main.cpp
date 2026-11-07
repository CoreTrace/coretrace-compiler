// SPDX-License-Identifier: Apache-2.0
//
// The lookups of the loader that the runtime makes while a module that links it detaches (#231).
// The program links the runtime and loads the DLL of reload/ct_runtime_library.cpp, which links a
// copy of its own. Each copy names a call, which starts its DbgHelp session; the copies also watch
// the loader. The program then replaces, in the import tables of both modules, GetModuleHandleW,
// GetProcAddress and SymFromAddr with functions that print each call they receive from then on:
// from the DLL, which the program no longer calls, and from the executable once its own TLS
// callback runs at detach, before those of its copy of the runtime. Then it leaves:
// - "free": it unloads the DLL, whose destructors name a call, then loads and unloads the DLL of
//   reload/ct_dllmain_library.cpp, which it had not loaded: the DLL's watch, were it still
//   registered, would report that load into its unloaded code;
// - "return": it returns from main with the DLL loaded;
// - "exitprocess": it calls ExitProcess with the DLL loaded.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstring>

extern "C" void __ct_vcall_trace(void* this_ptr, void* target, const char* site,
                                 const char* static_type);

namespace
{
    // The module whose import table holds a replacement.
    enum class Module
    {
        Dll,
        Exe,
    };

    // Set by the program's TLS callback when the executable detaches.
    volatile bool exe_detaching = false;

    decltype(&GetModuleHandleW) real_get_module_handle = nullptr;
    decltype(&GetProcAddress) real_get_proc_address = nullptr;
    decltype(&SymFromAddr) real_sym_from_addr = nullptr;

    // The name that the calls of `kModule` are printed with, or null while they are not.
    template <Module kModule> const char* printed()
    {
        if constexpr (kModule == Module::Dll)
        {
            return "dll";
        }
        else
        {
            return exe_detaching ? "exe" : nullptr;
        }
    }

    void append(char*& out, const char* end, const char* text)
    {
        while (*text != '\0' && out < end)
        {
            *out++ = *text++;
        }
    }

    // Writes to the standard output's handle, without the C runtime, which the process's exit may
    // have shut down.
    void print(const char* module, const char* function, const char* argument)
    {
        char line[512];
        char* out = line;
        const char* end = line + sizeof(line) - 1;
        append(out, end, "detach_lookups: ");
        append(out, end, module);
        append(out, end, " ");
        append(out, end, function);
        append(out, end, "(");
        append(out, end, argument);
        append(out, end, ")");
        *out++ = '\n';
        DWORD written = 0;
        (void)WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, static_cast<DWORD>(out - line),
                        &written, nullptr);
    }

    template <Module kModule> HMODULE WINAPI printGetModuleHandleW(LPCWSTR name)
    {
        if (const char* module = printed<kModule>())
        {
            char narrow[MAX_PATH] = "null";
            if (name)
            {
                size_t index = 0;
                for (; name[index] != L'\0' && index + 1 < sizeof(narrow); ++index)
                {
                    narrow[index] = static_cast<char>(name[index]);
                }
                narrow[index] = '\0';
            }
            print(module, "GetModuleHandleW", narrow);
        }
        return real_get_module_handle(name);
    }

    template <Module kModule> FARPROC WINAPI printGetProcAddress(HMODULE library, LPCSTR name)
    {
        if (const char* module = printed<kModule>())
        {
            print(module, "GetProcAddress", IS_INTRESOURCE(name) ? "ordinal" : name);
        }
        return real_get_proc_address(library, name);
    }

    template <Module kModule>
    BOOL WINAPI printSymFromAddr(HANDLE process, DWORD64 address, PDWORD64 displacement,
                                 PSYMBOL_INFO symbol)
    {
        if (const char* module = printed<kModule>())
        {
            print(module, "SymFromAddr", "");
        }
        return real_sym_from_addr(process, address, displacement, symbol);
    }

    // Points every import of `function` by `module` at `replacement`, and returns the function
    // that the last one pointed at: null when the module does not import it.
    void* replaceImport(HMODULE module, const char* function, void* replacement)
    {
        auto* base = reinterpret_cast<unsigned char*>(module);
        const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const IMAGE_DATA_DIRECTORY& imports =
            headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (imports.VirtualAddress == 0)
        {
            return nullptr;
        }
        void* previous = nullptr;
        for (const auto* descriptor =
                 reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
             descriptor->Name != 0; ++descriptor)
        {
            if (descriptor->OriginalFirstThunk == 0)
            {
                continue;
            }
            const auto* names =
                reinterpret_cast<const IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
            for (; names->u1.AddressOfData != 0; ++names, ++slots)
            {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                {
                    continue;
                }
                const auto* imported =
                    reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(imported->Name), function) != 0)
                {
                    continue;
                }
                DWORD protection = 0;
                if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE,
                                    &protection))
                {
                    return nullptr;
                }
                previous = reinterpret_cast<void*>(slots->u1.Function);
                slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
                (void)VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), protection,
                                     &protection);
            }
        }
        return previous;
    }

    template <typename Function>
    bool replace(HMODULE module, const char* name, Function replacement, Function& real)
    {
        void* previous = replaceImport(module, name, reinterpret_cast<void*>(replacement));
        if (previous && !real)
        {
            real = reinterpret_cast<Function>(previous);
        }
        return previous != nullptr;
    }

    // Both modules import GetModuleHandleW; the DLL names its calls with SymFromAddr.
    template <Module kModule> bool replaceLookups(HMODULE module)
    {
        const bool replaced = replace(module, "GetModuleHandleW", &printGetModuleHandleW<kModule>,
                                      real_get_module_handle);
        (void)replace(module, "GetProcAddress", &printGetProcAddress<kModule>,
                      real_get_proc_address);
        const bool names =
            replace(module, "SymFromAddr", &printSymFromAddr<kModule>, real_sym_from_addr) ||
            kModule == Module::Exe;
        return replaced && names;
    }

    void NTAPI markDetach(PVOID, DWORD reason, PVOID)
    {
        if (reason == DLL_PROCESS_DETACH)
        {
            exe_detaching = true;
            // Through the executable's import table: its line shows that the replacement is in
            // place when the executable's copy of the runtime detaches.
            (void)GetModuleHandleW(L"detach-lookups-control");
        }
    }

    struct Probe
    {
        virtual ~Probe() = default;
        virtual int value() const
        {
            return 1;
        }
    };

    Probe probe;

    int probeTarget()
    {
        return probe.value();
    }
} // namespace

// The program's TLS callback. .CRT$XLC comes before .CRT$XLY, where the runtime puts its own.
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:detach_lookups_tls_callback")
#pragma section(".CRT$XLC", read)
extern "C" __declspec(allocate(".CRT$XLC")) __attribute__((used))
const PIMAGE_TLS_CALLBACK detach_lookups_tls_callback = markDetach;

int main(int argc, char** argv)
{
    // Through the executable's copy of the runtime, which links its watch of the loader then.
    __ct_vcall_trace(&probe, reinterpret_cast<void*>(&probeTarget), "detach_lookups:1",
                     "<unknown>");

    HMODULE dll = LoadLibraryA(CT_RUNTIME_LIBRARY);
    auto* trace =
        dll ? reinterpret_cast<void (*)()>(GetProcAddress(dll, "ct_runtime_library_trace"))
            : nullptr;
    if (!trace)
    {
        std::puts("detach_lookups_main: the DLL did not load");
        return 2;
    }
    trace();

    if (!replaceLookups<Module::Dll>(dll) ||
        !replaceLookups<Module::Exe>(GetModuleHandleW(nullptr)))
    {
        std::puts("detach_lookups_main: an import was not replaced");
        return 3;
    }

    const char* how = argc > 1 ? argv[1] : "";
    if (std::strcmp(how, "free") == 0)
    {
        FreeLibrary(dll);
        HMODULE other = LoadLibraryA(CT_DLLMAIN_LIBRARY);
        if (!other)
        {
            std::puts("detach_lookups_main: the other DLL did not load");
            return 4;
        }
        FreeLibrary(other);
    }
    std::puts("detach_lookups_main: done");
    std::fflush(stdout);
    if (std::strcmp(how, "exitprocess") == 0)
    {
        ExitProcess(0);
    }
    return 0;
}
