// SPDX-License-Identifier: Apache-2.0
//
// Probe for #231, not to merge: whether a process that calls ExitProcess while other threads
// keep calling into the loader can hang in the detach of a DLL that links the runtime, whose
// TLS callbacks and destructors call GetModuleHandleW and GetProcAddress then. The parent runs
// the child many times and counts the children that do not exit in time, and the exit codes
// other than 0; "control" loads no DLL. Before it kills a child that did not exit, it prints
// the stacks of the child's threads. It exits with 3, so that ctest prints what it found.
#include <windows.h>

#include <dbghelp.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace
{
    // Exit codes of the child besides 0.
    constexpr int kNoDll = 2;
    constexpr int kThreadsNotStarted = 5;

    // The threads that have made a full round of loader calls.
    std::atomic<int> started{0};

    int child(const char* variant)
    {
        if (std::strcmp(variant, "runtime") == 0)
        {
            HMODULE runtime = LoadLibraryA(CT_RUNTIME_LIBRARY);
            FARPROC trace = runtime ? GetProcAddress(runtime, "ct_runtime_library_trace") : nullptr;
            if (!trace)
            {
                return kNoDll;
            }
            reinterpret_cast<void (*)()>(trace)();
        }
        // Threads that keep calling into the loader, as ExitProcess terminates them.
        constexpr int kThreads = 4;
        for (int index = 0; index < kThreads; ++index)
        {
            std::thread(
                []
                {
                    bool counted = false;
                    for (;;)
                    {
                        (void)GetModuleHandleW(L"ntdll.dll");
                        (void)GetModuleHandleW(L"kernel32.dll");
                        HMODULE version = LoadLibraryW(L"version.dll");
                        if (version)
                        {
                            FreeLibrary(version);
                        }
                        if (!counted)
                        {
                            counted = true;
                            started.fetch_add(1);
                        }
                    }
                })
                .detach();
        }
        // The 2 ms start once every thread is at work.
        for (int waited = 0; started.load() < kThreads; ++waited)
        {
            if (waited >= 5000)
            {
                ExitProcess(kThreadsNotStarted);
            }
            Sleep(1);
        }
        Sleep(2);
        ExitProcess(0);
    }

    // The stacks of the threads left in a child that did not exit, named from the modules'
    // exports, or from Microsoft's symbol server when DbgHelp can reach it.
    void printStacks(HANDLE process, DWORD process_id)
    {
#if defined(_M_X64) || defined(__x86_64__)
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const std::wstring path = L"srv*" + std::wstring(temp) +
                                  L"ct_symbols*https://msdl.microsoft.com/download/symbols";
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
        if (!SymInitializeW(process, path.c_str(), TRUE))
        {
            std::printf("probe-231 stacks: SymInitialize failed, error %lu\n", GetLastError());
            return;
        }
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(snapshot, &entry); more;
             more = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID != process_id)
            {
                continue;
            }
            HANDLE thread =
                OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                           FALSE, entry.th32ThreadID);
            if (!thread)
            {
                continue;
            }
            SuspendThread(thread);
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            std::printf("probe-231 thread %lu\n", entry.th32ThreadID);
            if (GetThreadContext(thread, &context))
            {
                STACKFRAME64 frame{};
                frame.AddrPC.Offset = context.Rip;
                frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = context.Rbp;
                frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Offset = context.Rsp;
                frame.AddrStack.Mode = AddrModeFlat;
                for (int depth = 0;
                     depth < 48 &&
                     StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context,
                                 nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
                     ++depth)
                {
                    const DWORD64 pc = frame.AddrPC.Offset;
                    IMAGEHLP_MODULE64 module{};
                    module.SizeOfStruct = sizeof(module);
                    const bool known = SymGetModuleInfo64(process, pc, &module) != FALSE;
                    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
                    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
                    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                    symbol->MaxNameLen = 255;
                    DWORD64 displacement = 0;
                    if (SymFromAddr(process, pc, &displacement, symbol))
                    {
                        std::printf("  %s!%s+0x%llx\n", known ? module.ModuleName : "?",
                                    symbol->Name, static_cast<unsigned long long>(displacement));
                    }
                    else
                    {
                        std::printf(
                            "  %s+0x%llx\n", known ? module.ModuleName : "?",
                            static_cast<unsigned long long>(known ? pc - module.BaseOfImage : pc));
                    }
                }
            }
            else
            {
                std::printf("  no context, error %lu\n", GetLastError());
            }
            CloseHandle(thread);
        }
        CloseHandle(snapshot);
        SymCleanup(process);
#else
        (void)process;
        (void)process_id;
        std::printf("probe-231 stacks: x64 only\n");
#endif
        std::fflush(stdout);
    }

    int parent(const char* variant, int runs)
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        wchar_t command[MAX_PATH + 64];
        std::swprintf(command, MAX_PATH + 64, L"\"%ls\" child %hs", self, variant);
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE null_device = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inherit,
                                         OPEN_EXISTING, 0, nullptr);
        int hangs = 0;
        int not_created = 0;
        std::map<DWORD, int> unexpected;
        int run = 0;
        for (; run < runs && hangs < 5; ++run)
        {
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = null_device;
            startup.hStdError = null_device;
            PROCESS_INFORMATION process{};
            std::vector<wchar_t> line(command, command + std::wcslen(command) + 1);
            if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                &startup, &process))
            {
                ++not_created;
                continue;
            }
            if (WaitForSingleObject(process.hProcess, 5000) == WAIT_TIMEOUT)
            {
                ++hangs;
                std::printf("probe-231 variant=%s run=%d: no exit within 5 s\n", variant, run);
                printStacks(process.hProcess, process.dwProcessId);
                TerminateProcess(process.hProcess, 9);
                WaitForSingleObject(process.hProcess, INFINITE);
            }
            else
            {
                DWORD code = 0;
                GetExitCodeProcess(process.hProcess, &code);
                if (code != 0)
                {
                    ++unexpected[code];
                }
            }
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        std::printf("probe-231 variant=%s runs=%d hangs=%d not_created=%d unexpected_exit_codes=",
                    variant, run, hangs, not_created);
        if (unexpected.empty())
        {
            std::printf("none");
        }
        for (const auto& [code, count] : unexpected)
        {
            std::printf("0x%lx:%d ", code, count);
        }
        std::printf("\n");
        std::fflush(stdout);
        return 3;
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc > 2 && std::strcmp(argv[1], "child") == 0)
    {
        return child(argv[2]);
    }
    if (argc > 3 && std::strcmp(argv[1], "parent") == 0)
    {
        return parent(argv[2], std::atoi(argv[3]));
    }
    return 2;
}
