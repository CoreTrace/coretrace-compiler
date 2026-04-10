// SPDX-License-Identifier: Apache-2.0
#include "ct_runtime_internal.h"

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
    std::once_flag ct_symbols_once;

    CT_NOINSTR void ct_ensure_symbols(void)
    {
        std::call_once(ct_symbols_once, []
        {
            HANDLE process = GetCurrentProcess();
            SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
            (void)SymInitialize(process, nullptr, TRUE);
        });
    }

    CT_NOINSTR void ct_write_stack_frame(HANDLE process, void* frame)
    {
        ct_write_prefix(CTLevel::Error);
        ct_write_cstr("  at ");
        ct_write_hex(reinterpret_cast<uintptr_t>(frame));

        char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        DWORD64 displacement = 0;
        if (SymFromAddr(process, reinterpret_cast<DWORD64>(frame), &displacement, symbol) != FALSE)
        {
            ct_write_cstr(" ");
            ct_write_cstr(symbol->Name);
        }

        ct_write_cstr("\n");
    }

    CT_NOINSTR LONG WINAPI ct_exception_filter(EXCEPTION_POINTERS* exception_info)
    {
        ct_disable_logging();
        ct_ensure_symbols();

        DWORD code = exception_info ? exception_info->ExceptionRecord->ExceptionCode
                                    : static_cast<DWORD>(0xFFFFFFFFu);

        ct_write_prefix(CTLevel::Error);
        ct_write_cstr("ct: fatal exception code=");
        ct_write_hex(static_cast<uintptr_t>(code));
        ct_write_cstr("\n");

        void* frames[64] = {};
        const USHORT count =
            CaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
        HANDLE process = GetCurrentProcess();
        for (USHORT index = 0; index < count; ++index)
        {
            ct_write_stack_frame(process, frames[index]);
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

    ct_ensure_symbols();
    SetUnhandledExceptionFilter(ct_exception_filter);

    ct_write_prefix(CTLevel::Info);
    ct_write_cstr("ct: backtrace handler installed\n");
}
