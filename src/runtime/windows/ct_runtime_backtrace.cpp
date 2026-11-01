// SPDX-License-Identifier: Apache-2.0
//
// The crash handler of this copy of the runtime, which CT_BACKTRACE installs for the whole process
// when the module that holds the copy initializes: the executable, or a DLL that links the runtime,
// whose copy's handler goes over the filter installed before it. When FreeLibrary unloads the DLL,
// whose code the process must not call afterwards, its copy puts that filter back (#236).
#include "ct_runtime_internal.h"
#include "ct_runtime_helpers.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

#pragma comment(lib, "Dbghelp.lib")

// The objects of this file are constructed before the program's and destroyed after them: a DLL's
// copy puts the filter back after the destructors of the DLL's global objects, whose crashes its
// handler still reports.
#pragma init_seg(lib)

// The section of the record below, in the module that holds this copy of the runtime.
#pragma section(".ctcrash", read, write)

namespace
{
    std::atomic<int> ct_backtrace_installed{0};

    // Runs inside the unhandled-exception filter, like the POSIX signal handler: only the
    // lock-free writers, and a symbol name only from a session, which the caller passes with the
    // DbgHelp lock held, and only for a module that the loader has not unloaded since the
    // session's modules were brought up to date (#225).
    CT_NOINSTR void ct_write_stack_frame(HANDLE process, void* frame)
    {
        ct_write_prefix_nolock(CTLevel::Error);
        ct_write_cstr("  at ");
        ct_write_hex(reinterpret_cast<uintptr_t>(frame));

        const DWORD64 address = reinterpret_cast<DWORD64>(frame);
        if (process && ct_dbghelp_module_current_locked(process, address))
        {
            char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;

            // Checked again once named: another thread may have had the loader unload the
            // module meanwhile.
            DWORD64 displacement = 0;
            if (SymFromAddr(process, address, &displacement, symbol) != FALSE &&
                ct_dbghelp_module_current_locked(process, address))
            {
                ct_write_cstr(" ");
                ct_write_cstr(symbol->Name);
            }
        }

        ct_write_cstr("\n");
    }

    CT_NOINSTR LONG WINAPI ct_exception_filter(EXCEPTION_POINTERS* exception_info)
    {
        ct_disable_logging();

        DWORD code = exception_info ? exception_info->ExceptionRecord->ExceptionCode
                                    : static_cast<DWORD>(0xFFFFFFFFu);

        ct_write_prefix_nolock(CTLevel::Error);
        ct_write_cstr("ct: fatal exception code=");
        ct_write_hex(static_cast<uintptr_t>(code));
        ct_write_cstr("\n");

        // Never wait for DbgHelp here: the faulting thread may be the one holding its lock,
        // and waiting would hang the process instead of letting it die. Nor start the session,
        // which ct_maybe_install_backtrace started, or bring its modules up to date, which lists
        // them. Without the lock or the session, frames are printed as bare addresses, and so
        // are those of the modules that the loader may have unloaded since.
        CtDbgHelpLock dbghelp(0);
        HANDLE process = dbghelp.owns() ? ct_dbghelp_started_session_locked() : nullptr;

        void* frames[64] = {};
        const USHORT count =
            CaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
        for (USHORT index = 0; index < count; ++index)
        {
            ct_write_stack_frame(process, frames[index]);
        }

        return EXCEPTION_EXECUTE_HANDLER;
    }

    // What a copy of the runtime tells the other copies in the process of its crash handler: the
    // filter that the handler went over when the copy installed it. A copy finds another one's
    // record in the section .ctcrash of the module that holds it. The magic number reads
    // "ctcrash", then the version of the layout.
    struct CtCrashHandlerRecord
    {
        std::uint64_t magic;
        std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> replaced;
    };

    constexpr std::uint64_t kCrashHandlerMagic = 0x0168736172637463;
    constexpr char kCrashHandlerSection[IMAGE_SIZEOF_SHORT_NAME] = {'.', 'c', 't', 'c',
                                                                    'r', 'a', 's', 'h'};

    __declspec(allocate(".ctcrash")) constinit CtCrashHandlerRecord ct_crash_handler_record{
        kCrashHandlerMagic, nullptr};

    // The record of the copy of the runtime in `module`, or null when the module holds none: it
    // links no copy, or one that keeps no record. The loader maps a module's headers with its image,
    // but a packer or an anti-tamper check may have erased them, or made them unreadable: each part
    // is read once found readable, and must look like what it is.
    CT_NOINSTR CtCrashHandlerRecord* ct_crash_handler_record_in(HMODULE module)
    {
        auto* image = reinterpret_cast<unsigned char*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
        if (!ct_is_readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
            dos->e_lfanew <= 0)
        {
            return nullptr;
        }
        const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
        if (!ct_is_readable(headers, sizeof(*headers)) || headers->Signature != IMAGE_NT_SIGNATURE)
        {
            return nullptr;
        }
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(headers);
        const WORD sections = headers->FileHeader.NumberOfSections;
        if (!ct_is_readable(section, sections * sizeof(IMAGE_SECTION_HEADER)))
        {
            return nullptr;
        }
        for (WORD index = 0; index < sections; ++index, ++section)
        {
            if (std::memcmp(section->Name, kCrashHandlerSection, sizeof(section->Name)) != 0)
            {
                continue;
            }
            auto* record = reinterpret_cast<CtCrashHandlerRecord*>(image + section->VirtualAddress);
            if ((section->Characteristics & IMAGE_SCN_MEM_WRITE) == 0 ||
                section->Misc.VirtualSize < sizeof(CtCrashHandlerRecord) ||
                !ct_is_readable(record, sizeof(*record)))
            {
                return nullptr;
            }
            return record->magic == kCrashHandlerMagic ? record : nullptr;
        }
        return nullptr;
    }

    // Gives `replaced`, the filter that this copy's handler went over, to every copy of the runtime
    // whose handler went over this one's, so that none puts back a handler whose code is gone when
    // its own DLL unloads. That is one copy at most, unless the program put back, over the handlers
    // of copies loaded since, a filter it had read earlier. A filter of the program installed over
    // such a copy's handler hides it from the process's filter: the copies are looked for among
    // the modules that the loader lists, in a block of the process's heap, which DllMain may
    // allocate. FreeLibrary holds the loader lock meanwhile, so that no thread unloads one.
    CT_NOINSTR void ct_crash_handler_hand_over(LPTOP_LEVEL_EXCEPTION_FILTER replaced)
    {
        const HANDLE process = GetCurrentProcess();
        HMODULE first = nullptr;
        DWORD size = 0;
        if (!EnumProcessModules(process, &first, sizeof(first), &size) || size == 0)
        {
            return;
        }
        auto* modules = static_cast<HMODULE*>(HeapAlloc(GetProcessHeap(), 0, size));
        DWORD listed = 0;
        if (modules && EnumProcessModules(process, modules, size, &listed))
        {
            const DWORD count = static_cast<DWORD>(std::min(size, listed) / sizeof(HMODULE));
            for (DWORD index = 0; index < count; ++index)
            {
                CtCrashHandlerRecord* above = ct_crash_handler_record_in(modules[index]);
                LPTOP_LEVEL_EXCEPTION_FILTER handler = &ct_exception_filter;
                if (above)
                {
                    (void)above->replaced.compare_exchange_strong(handler, replaced,
                                                                  std::memory_order_acq_rel);
                }
            }
        }
        if (modules)
        {
            HeapFree(GetProcessHeap(), 0, modules);
        }
    }

    // Puts back the filter that this copy's handler went over when FreeLibrary unloads the DLL that
    // holds the copy, if the handler is still the process's: a filter installed over it since
    // stays. The copies whose handlers went over this one's get that filter instead. Windows has no
    // call that reads the process's filter without setting one: a filter that another thread
    // installs between the two calls below is replaced. Nothing changes when the process
    // terminates, nor while a DLL's own exit() runs this destructor, before the loader detaches the
    // DLL and without the loader lock: the DLL stays mapped until the process ends.
    struct CtCrashHandlerRelease
    {
        // User-provided, so that the object is initialized dynamically, in this file's
        // init_seg: clang registers the destructor of a constant-initialized object from
        // .CRT$XCU, with the program's own objects.
        CT_NOINSTR CtCrashHandlerRelease() {}

        CT_NOINSTR ~CtCrashHandlerRelease()
        {
            if (ct_backtrace_installed.load(std::memory_order_acquire) == 0 ||
                !ct_dll_unloading_dynamically() || !ct_loader_detaching_module())
            {
                return;
            }
            const LPTOP_LEVEL_EXCEPTION_FILTER replaced =
                ct_crash_handler_record.replaced.load(std::memory_order_acquire);
            const LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(replaced);
            if (current != &ct_exception_filter)
            {
                SetUnhandledExceptionFilter(current);
            }
            ct_crash_handler_hand_over(replaced);
        }
    };

    CtCrashHandlerRelease ct_crash_handler_release;
} // namespace

CT_NOINSTR void ct_maybe_install_backtrace(void)
{
    if (std::getenv("CT_BACKTRACE") == nullptr)
    {
        return;
    }

    int expected = 0;
    if (!ct_backtrace_installed.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
    {
        return;
    }

    {
        // The session starts now rather than in the filter, where the heap may be corrupt, with
        // its modules up to date: the filter names the frames of those that the loader has not
        // unloaded since. Listing them allocates for the runtime itself (#197).
        CtRuntimeScope scope;
        CtDbgHelpLock dbghelp(CtDbgHelpLock::kLookupWait);
        HANDLE process = dbghelp.owns() ? ct_dbghelp_session_locked() : nullptr;
        if (process)
        {
            (void)ct_dbghelp_sync_modules_locked(process);
        }
    }
    ct_crash_handler_record.replaced.store(SetUnhandledExceptionFilter(&ct_exception_filter),
                                           std::memory_order_release);

    ct_write_prefix(CTLevel::Info);
    ct_write_cstr("ct: backtrace handler installed\n");
}
