// SPDX-License-Identifier: Apache-2.0
//
// The DbgHelp state of this copy of the runtime (ct_runtime_helpers.h): its handle of the DbgHelp
// lock of the process, and its session. Whoever uses them goes through this file, which releases
// them when FreeLibrary unloads the DLL that holds this copy, after the DLL's last virtual calls
// (#208), and leaves them to the system when the process terminates (#226). It also watches the
// loader: before a lookup, the session drops the modules of the DLLs unloaded and lists those
// loaded (#208), and the crash handler, which cannot list them, prints bare the frames of the
// modules unloaded since (#225).
#include "ct_runtime_helpers.h"
#include "ct_runtime_internal.h"

#include <atomic>
#include <cwchar>
#include <vector>

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
                ct_process_shutdown() != CtProcessShutdown::NotInProgress)
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
} // namespace

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

namespace
{
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
        // The base of the executable, which stays loaded as long as the process.
        std::atomic<ULONG_PTR> executable{0};
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
        ct_module_changes.executable.store(reinterpret_cast<ULONG_PTR>(GetModuleHandleW(nullptr)),
                                           std::memory_order_release);
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
} // namespace

// DbgHelp keeps the modules it has listed: a DLL unloaded, then replaced by another one at
// the same base, even at the same path with the same time stamp, kept the first one's names,
// and a DLL loaded since was unknown (#208). Only the loader tells such loads apart. Once it
// has loaded or unloaded a module, the session drops the modules at the bases unloaded, then
// lists the modules loaded now. It drops every module it has instead the first time, for what
// happened before the watch, without the watch, and when more DLLs were unloaded than the
// ring keeps. The session itself is kept. The modules count as current only once every step
// has succeeded: otherwise the function returns false, the lookup names nothing, and the next
// one starts again from the same point. Call with the DbgHelp lock held.
CT_NOINSTR bool ct_dbghelp_sync_modules_locked(HANDLE process)
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
    recover = recover || changes.unloads.load(std::memory_order_acquire) - changes.synced_unloads >=
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

// Whether the session's module at `address` is still the one the loader has there, told without
// listing the modules, which the crash handler cannot do: they were brought up to date, the
// watch is still on, and no DLL unloaded since was at that module's base. Once the watch has
// stopped, at detach, the loader no longer reports what it unloads: `watched` stays true for the
// lookups of the module's destructors, but no module counts as current here. The ring is read as
// ct_dbghelp_sync_modules_locked reads it, and a module counts as current only while fewer DLLs
// were unloaded since than it keeps, but for the executable and the module that holds this copy,
// which the loader cannot have unloaded. Call with the DbgHelp lock held.
CT_NOINSTR bool ct_dbghelp_module_current_locked(HANDLE process, DWORD64 address)
{
    const CtModuleChanges& changes = ct_module_changes;
    if (!changes.synced || !changes.cookie.load(std::memory_order_acquire))
    {
        return false;
    }
    const DWORD64 base = SymGetModuleBase64(process, address);
    if (base == 0)
    {
        return false;
    }
    // The executable stays loaded as long as the process, and the module that holds this copy as
    // long as its code runs: the session's modules at their bases stay theirs, however many DLLs
    // the loader has unloaded since.
    if (base == changes.executable.load(std::memory_order_acquire) ||
        base == reinterpret_cast<ULONG_PTR>(&__ImageBase))
    {
        return true;
    }
    const unsigned long unloads = changes.unloads.load(std::memory_order_acquire);
    if (unloads - changes.synced_unloads >= CtModuleChanges::kUnloadedBases)
    {
        return false;
    }
    for (unsigned long index = changes.synced_unloads; index != unloads; ++index)
    {
        if (changes.unloaded[index % CtModuleChanges::kUnloadedBases].load(
                std::memory_order_acquire) == base)
        {
            return false;
        }
    }
    return changes.unloads.load(std::memory_order_acquire) - changes.synced_unloads <
           CtModuleChanges::kUnloadedBases;
}

// The TLS callback of the watch, in the file of the session, which the virtual call boxes and the
// crash handler both use: a module that links either watches the loader.
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:ct_module_changes_tls_callback")
#pragma section(".CRT$XLY", read)
extern "C" __declspec(allocate(".CRT$XLY")) __attribute__((used))
const PIMAGE_TLS_CALLBACK ct_module_changes_tls_callback = ct_module_changes_tls;
