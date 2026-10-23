// SPDX-License-Identifier: Apache-2.0
//
// The DbgHelp state of this copy of the runtime (ct_runtime_helpers.h): its handle of the DbgHelp
// lock of the process, and its session. Whoever uses them goes through this file, which releases
// them when FreeLibrary unloads the DLL that holds this copy, after the DLL's last virtual calls
// (#208), and leaves them to the system when the process terminates (#226).
#include "ct_runtime_helpers.h"
#include "ct_runtime_internal.h"

#include <atomic>
#include <cwchar>

// The objects of this file are constructed before the program's and destroyed after them: the
// release comes after the destructors of the program's global objects, and their virtual calls.
#pragma init_seg(lib)

// The module that holds this copy of the runtime.
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
    // Opened at the first use; null once released, as when it could not be opened.
    std::atomic<HANDLE> ct_semaphore{nullptr};
    // Once released, nothing opens the semaphore or starts a session again, and DbgHelp is not
    // called: the module is unloading.
    std::atomic<bool> ct_released{false};
    // Used with the DbgHelp lock held.
    HANDLE ct_session = nullptr;
    // Set by this file's TLS callback when the module detaches because the process terminates.
    // A module's TLS callbacks run before its destructors at detach, which #208's tests rely on.
    std::atomic<bool> ct_process_terminating{false};

    CT_NOINSTR HANDLE ct_open_semaphore()
    {
        if (ct_released.load(std::memory_order_acquire))
        {
            return nullptr;
        }
        HANDLE handle = ct_semaphore.load(std::memory_order_acquire);
        if (handle)
        {
            return handle;
        }
        wchar_t name[64];
        (void)swprintf(name, sizeof(name) / sizeof(name[0]), L"Local\\coretrace-dbghelp-%lu",
                       GetCurrentProcessId());
        HANDLE created = CreateSemaphoreW(nullptr, 1, 1, name);
        if (!created)
        {
            return nullptr;
        }
        if (!ct_semaphore.compare_exchange_strong(handle, created, std::memory_order_acq_rel))
        {
            (void)CloseHandle(created);
            return handle;
        }
        return created;
    }

    // Releases the session and the semaphore's handle of a DLL's copy when FreeLibrary unloads
    // the DLL, after the destructors of its global objects, which may still name calls. SymCleanup
    // needs DbgHelp: when another thread holds it past the lookups' wait, as one that waits for
    // the loader lock FreeLibrary holds, the session is left as it is. The executable keeps its
    // state until the process ends: code of the DLLs that detach after its destructors may still
    // call into it. Neither does a DLL release anything when the process terminates: the
    // guidance for DllMain, which applies to the destructors the C runtime runs from it, is to
    // free resources only when the DLL is unloaded dynamically, and not to call FreeLibrary,
    // which SymCleanup may do, during process termination.
    struct CtDbgHelpRelease
    {
        // User-provided, so that the object is initialized dynamically, in this file's
        // init_seg: clang registers the destructor of a constant-initialized object from
        // .CRT$XCU, with the program's own objects.
        CT_NOINSTR CtDbgHelpRelease() {}

        CT_NOINSTR ~CtDbgHelpRelease()
        {
            if (reinterpret_cast<HMODULE>(&__ImageBase) == GetModuleHandleW(nullptr) ||
                ct_process_terminating.load(std::memory_order_acquire))
            {
                return;
            }
            {
                CtDbgHelpLock dbghelp(CtDbgHelpLock::kLookupWait);
                ct_released.store(true, std::memory_order_release);
                if (dbghelp.owns() && ct_session)
                {
                    (void)SymCleanup(ct_session);
                    (void)CloseHandle(ct_session);
                    ct_session = nullptr;
                }
            }
            HANDLE semaphore = ct_semaphore.exchange(nullptr);
            if (semaphore)
            {
                (void)CloseHandle(semaphore);
            }
        }
    };

    CtDbgHelpRelease ct_dbghelp_release;

    // At DLL_PROCESS_DETACH, lpvReserved is null when FreeLibrary unloads the module, and not
    // when the process terminates (DllMain's documentation).
    CT_NOINSTR void NTAPI ct_dbghelp_tls(PVOID, DWORD reason, PVOID reserved)
    {
        if (reason == DLL_PROCESS_DETACH && reserved != nullptr)
        {
            ct_process_terminating.store(true, std::memory_order_release);
        }
    }
} // namespace

// The TLS callback that tells the release a termination from an unload, in the file of the
// release, so that a module linking one links the other.
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:ct_dbghelp_tls_callback")
#pragma section(".CRT$XLY", read)
extern "C" __declspec(allocate(".CRT$XLY")) __attribute__((used))
const PIMAGE_TLS_CALLBACK ct_dbghelp_tls_callback = ct_dbghelp_tls;

CT_NOINSTR CtDbgHelpLock::CtDbgHelpLock(DWORD wait) : semaphore_(ct_open_semaphore())
{
    thread_local bool waited_in_vain = false;
    if (!semaphore_)
    {
        return;
    }
    owns_ = WaitForSingleObject(semaphore_, waited_in_vain ? 0 : wait) == WAIT_OBJECT_0;
    waited_in_vain = !owns_;
}

CT_NOINSTR CtDbgHelpLock::~CtDbgHelpLock()
{
    if (owns_)
    {
        (void)ReleaseSemaphore(semaphore_, 1, nullptr);
    }
}

CT_NOINSTR HANDLE ct_dbghelp_session_locked()
{
    if (ct_session || ct_released.load(std::memory_order_acquire))
    {
        return ct_session;
    }
    HANDLE process = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &process, 0,
                         FALSE, DUPLICATE_SAME_ACCESS))
    {
        return nullptr;
    }
    // DbgHelp's options are the process's: the runtime adds its own to the program's.
    SymSetOptions(SymGetOptions() | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    if (!SymInitialize(process, nullptr, TRUE))
    {
        (void)CloseHandle(process);
        return nullptr;
    }
    ct_session = process;
    return process;
}

CT_NOINSTR HANDLE ct_dbghelp_started_session_locked()
{
    return ct_session;
}
