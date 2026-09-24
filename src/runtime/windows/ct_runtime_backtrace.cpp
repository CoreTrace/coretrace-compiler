// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_internal.h"
#include "ct_runtime_helpers.h"

#include <atomic>
#include <cstdlib>
#include <iterator>
#include <mutex>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#pragma comment(lib, "Dbghelp.lib")

namespace
{
    std::atomic<int> ct_backtrace_installed{0};

    // Runs inside the unhandled-exception filter, like the POSIX signal handler: only the
    // lock-free writers, and a symbol name only when the caller holds the DbgHelp lock.
    CT_NOINSTR void ct_write_stack_frame(HANDLE process, void* frame, bool symbolize)
    {
        ct_write_prefix_nolock(CTLevel::Error);
        ct_write_cstr("  at ");
        ct_write_hex(reinterpret_cast<uintptr_t>(frame));

        if (symbolize)
        {
            char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;

            DWORD64 displacement = 0;
            if (SymFromAddr(process, reinterpret_cast<DWORD64>(frame), &displacement, symbol) !=
                FALSE)
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
        // and waiting would hang the process instead of letting it die. Without the lock,
        // frames are printed as bare addresses.
        std::unique_lock<std::mutex> dbghelp(ct_dbghelp_mutex(), std::try_to_lock);
        if (dbghelp.owns_lock())
        {
            ct_dbghelp_initialize_locked();
        }

        void* frames[64] = {};
        const USHORT count =
            CaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
        HANDLE process = GetCurrentProcess();
        for (USHORT index = 0; index < count; ++index)
        {
            ct_write_stack_frame(process, frames[index], dbghelp.owns_lock());
        }

        return EXCEPTION_EXECUTE_HANDLER;
    }
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
        // Symbols are loaded now rather than in the filter, where the heap may be corrupt.
        std::lock_guard<std::mutex> dbghelp(ct_dbghelp_mutex());
        ct_dbghelp_initialize_locked();
    }
    SetUnhandledExceptionFilter(ct_exception_filter);

    ct_write_prefix(CTLevel::Info);
    ct_write_cstr("ct: backtrace handler installed\n");
}
