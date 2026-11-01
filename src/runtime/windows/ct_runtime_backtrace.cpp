// SPDX-License-Identifier: Apache-2.0
//
// The crash handler of this copy of the runtime, which CT_BACKTRACE installs for the whole process
// when the module that holds the copy initializes: the executable, or a DLL that links the runtime,
// whose copy's handler goes over the filter installed before it. When FreeLibrary unloads the DLL,
// whose code the process must not call afterwards, its copy puts that filter back (#236).
#include "ct_runtime_internal.h"
#include "ct_runtime_helpers.h"

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

#pragma comment(lib, "Dbghelp.lib")

// The module that holds this copy of the runtime.
extern "C" IMAGE_DOS_HEADER __ImageBase;

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

    // What a copy of the runtime tells the other copies in the process of its crash handler, in the
    // section .ctcrash of its module, where a copy finds another one's record from the address of
    // its handler. `replaced` is the filter that the handler went over when the copy installed it,
    // and `below` the record of the copy whose handler that filter is, if any. `above` is the first
    // of the copies whose handlers went over this one's, and `next` the following one over the same
    // copy. A DLL's copy makes its links when it installs its handler and undoes them when
    // FreeLibrary unloads the DLL, both with the loader lock held: a record is linked only while its
    // module is loaded, and the release follows the links without allocating or listing the
    // modules. The executable's copy, which installs its handler without the loader lock and keeps
    // it as long as the process, links to none. The magic number reads "ctcrash", then the version
    // of the layout.
    struct CtCrashHandlerRecord
    {
        std::uint64_t magic;
        LPTOP_LEVEL_EXCEPTION_FILTER handler;
        LPTOP_LEVEL_EXCEPTION_FILTER replaced;
        CtCrashHandlerRecord* below;
        CtCrashHandlerRecord* above;
        CtCrashHandlerRecord* next;
    };

    constexpr std::uint64_t kCrashHandlerMagic = 0x0268736172637463;
    constexpr char kCrashHandlerSection[IMAGE_SIZEOF_SHORT_NAME] = {'.', 'c', 't', 'c',
                                                                    'r', 'a', 's', 'h'};

    __declspec(allocate(".ctcrash")) constinit CtCrashHandlerRecord ct_crash_handler_record{
        kCrashHandlerMagic, &ct_exception_filter, nullptr, nullptr, nullptr, nullptr};

    // Whether this copy's handler is installed, with its record: set once, when the module
    // initializes, and read when it detaches.
    bool ct_crash_handler_in_place = false;

    // The record of the copy of the runtime whose handler `filter` is, if any. `readable` is false
    // when the headers of the module that holds `filter` cannot be read, so that whether it is a
    // copy's handler cannot be told: a packer or an anti-tamper check may have erased them, or made
    // them unreadable. Each part is read once found readable, and must look like what it is.
    CT_NOINSTR CtCrashHandlerRecord* ct_crash_handler_record_of(LPTOP_LEVEL_EXCEPTION_FILTER filter,
                                                                bool& readable)
    {
        readable = true;
        HMODULE module = nullptr;
        if (!filter || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           reinterpret_cast<LPCWSTR>(filter), &module))
        {
            return nullptr;
        }
        readable = false;
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
            readable = true;
            return record->magic == kCrashHandlerMagic && record->handler == filter ? record
                                                                                    : nullptr;
        }
        readable = true;
        return nullptr;
    }

    // Takes `record` out of the links when FreeLibrary unloads the DLL that holds its copy: out of
    // the list of the copy below, and the copies above get the filter that its handler replaced,
    // and that copy below as theirs, so that none puts back the unloading copy's handler, whose
    // code is gone, when its own DLL unloads. FreeLibrary holds the loader lock meanwhile: every
    // record linked is in a module still loaded.
    CT_NOINSTR void ct_crash_handler_unlink(CtCrashHandlerRecord& record)
    {
        if (record.below)
        {
            for (CtCrashHandlerRecord** link = &record.below->above; *link; link = &(*link)->next)
            {
                if (*link == &record)
                {
                    *link = record.next;
                    break;
                }
            }
        }
        for (CtCrashHandlerRecord* above = record.above; above;)
        {
            CtCrashHandlerRecord* const next = above->next;
            above->replaced = record.replaced;
            above->below = record.below;
            above->next = record.below ? record.below->above : nullptr;
            if (record.below)
            {
                record.below->above = above;
            }
            above = next;
        }
        record.below = nullptr;
        record.above = nullptr;
        record.next = nullptr;
    }

    // Puts back the filter that this copy's handler went over when FreeLibrary unloads the DLL that
    // holds the copy, if the handler is still the process's: a filter installed over it since
    // stays. Then takes the copy's record out of the links. Windows has no call that reads the
    // process's filter without setting one: a filter that another thread installs between the two
    // calls below is replaced. Nothing changes when the process terminates, nor while a DLL's own
    // exit() runs this destructor, before the loader detaches the DLL and without the loader lock:
    // the DLL stays mapped until the process ends.
    struct CtCrashHandlerRelease
    {
        // User-provided, so that the object is initialized dynamically, in this file's
        // init_seg: clang registers the destructor of a constant-initialized object from
        // .CRT$XCU, with the program's own objects.
        CT_NOINSTR CtCrashHandlerRelease() {}

        CT_NOINSTR ~CtCrashHandlerRelease()
        {
            if (!ct_crash_handler_in_place || !ct_dll_unloading_dynamically() ||
                !ct_loader_detaching_module())
            {
                return;
            }
            CtCrashHandlerRecord& record = ct_crash_handler_record;
            const LPTOP_LEVEL_EXCEPTION_FILTER current =
                SetUnhandledExceptionFilter(record.replaced);
            if (current != record.handler)
            {
                SetUnhandledExceptionFilter(current);
            }
            ct_crash_handler_unlink(record);
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
    CtCrashHandlerRecord& record = ct_crash_handler_record;
    record.replaced = SetUnhandledExceptionFilter(record.handler);
    if (reinterpret_cast<HMODULE>(&__ImageBase) != GetModuleHandleW(nullptr))
    {
        // A DLL initializes with the loader lock held, which keeps the links of every copy.
        bool readable = true;
        record.below = ct_crash_handler_record_of(record.replaced, readable);
        if (record.below)
        {
            record.next = record.below->above;
            record.below->above = &record;
        }
    }
    ct_crash_handler_in_place = true;

    ct_write_prefix(CTLevel::Info);
    ct_write_cstr("ct: backtrace handler installed\n");
}
