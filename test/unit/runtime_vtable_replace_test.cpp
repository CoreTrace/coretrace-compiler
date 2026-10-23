// SPDX-License-Identifier: Apache-2.0
//
// On Windows, a virtual call box names its target through DbgHelp, which keeps the modules it
// has listed. A DLL replaced by another one at the same path, with the same time stamp, as
// reproducible builds give, must not keep the first one's names (#208): two builds of one
// library, their time stamps set to zero, are copied in turn to one path and loaded from there.
// The libraries are built from reload/ct_reload_library.cpp. A DLL loaded after DbgHelp listed
// the modules must be named too, after many unloads as well. The runtime uses DbgHelp in a session
// of its own and under the DbgHelp lock of the process, which every copy of it takes, the copy in
// a DLL built from reload/ct_runtime_library.cpp included; that copy watches the loader until it
// unloads. A lookup made under the loader lock, from reload/ct_dllmain_library.cpp, waits for no
// one forever.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>
#include <dbghelp.h>

#pragma comment(lib, "Dbghelp.lib")

extern "C"
{
    void __ct_vcall_trace(void* this_ptr, void* target, const char* site, const char* static_type);
}

namespace
{
    namespace fs = std::filesystem;

    // Copies the DLL at `from` to `to`, with the time stamp of its PE header set to zero.
    bool copyWithoutTimeStamp(const fs::path& from, const fs::path& to)
    {
        std::ifstream in(from, std::ios::binary);
        std::vector<char> image((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        LONG headers = 0;
        if (image.size() < sizeof(IMAGE_DOS_HEADER))
        {
            return false;
        }
        std::memcpy(&headers, image.data() + offsetof(IMAGE_DOS_HEADER, e_lfanew), sizeof(headers));
        const size_t stamp = static_cast<size_t>(headers) + offsetof(IMAGE_NT_HEADERS, FileHeader) +
                             offsetof(IMAGE_FILE_HEADER, TimeDateStamp);
        if (headers < 0 || stamp + sizeof(DWORD) > image.size())
        {
            return false;
        }
        const DWORD zero = 0;
        std::memcpy(image.data() + stamp, &zero, sizeof(zero));
        std::ofstream out(to, std::ios::binary | std::ios::trunc);
        out.write(image.data(), static_cast<std::streamsize>(image.size()));
        return static_cast<bool>(out);
    }

    void* callEntry(HMODULE library, const char* name)
    {
        auto* entry = reinterpret_cast<void* (*)()>(GetProcAddress(library, name));
        return entry ? entry() : nullptr;
    }

    // The box of a virtual call to the function of the library.
    std::string traceACall(HMODULE library, const char* site)
    {
        void* object = callEntry(library, "ct_reload_object");
        void* target = callEntry(library, "ct_reload_target");
        testing::internal::CaptureStderr();
        __ct_vcall_trace(object, target, site, "<unknown>");
        return testing::internal::GetCapturedStderr();
    }

    // The `symbol` line of the first box of `site` from `from` on, empty without one.
    std::string symbolOf(const std::string& boxes, const char* site, size_t from = 0)
    {
        const size_t box = boxes.find(site, from);
        const size_t line = box == std::string::npos ? box : boxes.find("symbol", box);
        if (line == std::string::npos)
        {
            return std::string();
        }
        return boxes.substr(line, boxes.find('\n', line) - line);
    }

    bool names(const std::string& symbol, const char* function)
    {
        return symbol.find(function) != std::string::npos;
    }

    bool namesNothing(const std::string& symbol)
    {
        return symbol.find("<unknown>") != std::string::npos;
    }

    // The name of the DbgHelp lock of the process.
    std::wstring dbgHelpLockName()
    {
        wchar_t name[64];
        (void)swprintf(name, sizeof(name) / sizeof(name[0]), L"Local\\coretrace-dbghelp-%lu",
                       GetCurrentProcessId());
        return name;
    }

    // Sends the standard error of the DLLs loaded from now on to a file: the copy of the runtime
    // in a DLL writes through the DLL's own C runtime, which takes the handle when it loads.
    class DllStandardError
    {
      public:
        explicit DllStandardError(fs::path path)
            : path_(std::move(path)), saved_(GetStdHandle(STD_ERROR_HANDLE))
        {
            file_ = CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file_ != INVALID_HANDLE_VALUE)
            {
                SetStdHandle(STD_ERROR_HANDLE, file_);
            }
        }
        ~DllStandardError()
        {
            restore();
        }
        DllStandardError(const DllStandardError&) = delete;
        DllStandardError& operator=(const DllStandardError&) = delete;

        // What the DLLs wrote, once they are unloaded.
        std::string contents()
        {
            restore();
            std::ifstream in(path_, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        }

      private:
        void restore()
        {
            if (file_ != INVALID_HANDLE_VALUE)
            {
                SetStdHandle(STD_ERROR_HANDLE, saved_);
                CloseHandle(file_);
                file_ = INVALID_HANDLE_VALUE;
            }
        }

        fs::path path_;
        HANDLE saved_;
        HANDLE file_ = INVALID_HANDLE_VALUE;
    };

    TEST(VtableReplace, ADllReplacedAtItsPathWithItsTimeStampIsNamedAgain)
    {
        const fs::path path = fs::path(testing::TempDir()) / "ct_replaced_library.dll";
        struct Build
        {
            const char* source;
            const char* name;
            const char* other;
        };
        const Build builds[] = {{CT_RELOAD_LIBRARY_A, "reload_a", "reload_b"},
                                {CT_RELOAD_LIBRARY_B, "reload_b", "reload_a"},
                                {CT_RELOAD_LIBRARY_A, "reload_a", "reload_b"}};
        HMODULE first = nullptr;
        for (const Build& build : builds)
        {
            SCOPED_TRACE(build.name);
            ASSERT_TRUE(copyWithoutTimeStamp(build.source, path)) << path;
            HMODULE library = LoadLibraryW(path.c_str());
            ASSERT_NE(library, nullptr) << path;
            // Each build at the base of the first, by which DbgHelp lists the modules.
            if (!first)
            {
                first = library;
            }
            EXPECT_EQ(library, first);
            void* object = callEntry(library, "ct_reload_object");
            void* target = callEntry(library, "ct_reload_target");

            testing::internal::CaptureStderr();
            __ct_vcall_trace(object, target, "replace:1", "<unknown>");
            const std::string box = testing::internal::GetCapturedStderr();
            FreeLibrary(library);

            // DbgHelp names the function without its parameters.
            EXPECT_NE(box.find(std::string(build.name) + "::describe"), std::string::npos) << box;
            EXPECT_EQ(box.find(build.other), std::string::npos) << box;
        }
        std::error_code ignored;
        fs::remove(path, ignored);
    }

    TEST(VtableReplace, ADllLoadedAfterALookupIsNamed)
    {
        HMODULE first = LoadLibraryA(CT_RELOAD_LIBRARY_A);
        ASSERT_NE(first, nullptr) << CT_RELOAD_LIBRARY_A;
        const std::string before = traceACall(first, "replace:2");
        HMODULE late = LoadLibraryA(CT_RELOAD_LIBRARY_B);
        ASSERT_NE(late, nullptr) << CT_RELOAD_LIBRARY_B;
        const std::string after = traceACall(late, "replace:3");
        FreeLibrary(late);
        FreeLibrary(first);

        EXPECT_NE(before.find("reload_a::describe"), std::string::npos) << before;
        EXPECT_NE(after.find("reload_b::describe"), std::string::npos) << after;
    }

    // The runtime keeps the bases of the last 64 DLLs unloaded: the replaced DLL's base is
    // among the older ones, once twice as many DLLs were unloaded at other bases since.
    TEST(VtableReplace, ADllReplacedAfterManyUnloadsIsNamedAgain)
    {
        constexpr int kUnloads = 2 * 64;
        const fs::path path = fs::path(testing::TempDir()) / "ct_replaced_after_unloads.dll";
        ASSERT_TRUE(copyWithoutTimeStamp(CT_RELOAD_LIBRARY_A, path)) << path;
        HMODULE first = LoadLibraryW(path.c_str());
        ASSERT_NE(first, nullptr) << path;
        const std::string before = traceACall(first, "replace:4");
        FreeLibrary(first);

        ASSERT_TRUE(copyWithoutTimeStamp(CT_RELOAD_LIBRARY_B, path)) << path;
        HMODULE replaced = LoadLibraryW(path.c_str());
        ASSERT_NE(replaced, nullptr) << path;
        EXPECT_EQ(replaced, first);
        // Loaded and unloaded while the replaced DLL holds the first one's base.
        for (int unload = 0; unload < kUnloads; ++unload)
        {
            HMODULE other = LoadLibraryA(CT_RELOAD_LIBRARY_A);
            ASSERT_NE(other, nullptr) << CT_RELOAD_LIBRARY_A;
            FreeLibrary(other);
        }
        const std::string after = traceACall(replaced, "replace:5");
        FreeLibrary(replaced);
        std::error_code ignored;
        fs::remove(path, ignored);

        EXPECT_NE(before.find("reload_a::describe"), std::string::npos) << before;
        EXPECT_NE(after.find("reload_b::describe"), std::string::npos) << after;
        EXPECT_EQ(after.find("reload_a"), std::string::npos) << after;
    }

    // A program that uses DbgHelp itself, on GetCurrentProcess() as most do, keeps its session,
    // with a module the loader does not know, while the runtime lists, drops and lists again the
    // modules of its own.
    TEST(VtableReplace, AProgramsOwnDbgHelpSessionKeepsItsModules)
    {
        HANDLE program = GetCurrentProcess();
        constexpr DWORD64 kBase = 0x10000000;
        constexpr DWORD64 kSymbol = kBase + 0x100;
        ASSERT_TRUE(SymInitialize(program, nullptr, FALSE)) << GetLastError();
        ASSERT_NE(SymLoadModuleExW(program, nullptr, L"ct_program_module", nullptr, kBase, 0x1000,
                                   nullptr, SLMFLAG_VIRTUAL),
                  0u)
            << GetLastError();
        ASSERT_TRUE(SymAddSymbolW(program, kBase, L"ct_program_symbol", kSymbol, 0x10, 0))
            << GetLastError();

        const fs::path path = fs::path(testing::TempDir()) / "ct_program_session.dll";
        std::string boxes;
        for (const char* source : {CT_RELOAD_LIBRARY_A, CT_RELOAD_LIBRARY_B})
        {
            ASSERT_TRUE(copyWithoutTimeStamp(source, path)) << path;
            HMODULE library = LoadLibraryW(path.c_str());
            ASSERT_NE(library, nullptr) << path;
            boxes += traceACall(library, "replace:6");
            FreeLibrary(library);
        }

        char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        const bool found = SymFromAddr(program, kSymbol, &displacement, symbol) != FALSE;
        const std::string name = found ? symbol->Name : "";
        SymCleanup(program);
        std::error_code ignored;
        fs::remove(path, ignored);

        EXPECT_EQ(name, "ct_program_symbol");
        EXPECT_NE(boxes.find("reload_a::describe"), std::string::npos) << boxes;
        EXPECT_NE(boxes.find("reload_b::describe"), std::string::npos) << boxes;
    }

    // Every copy of the runtime calls DbgHelp under the lock of the process, which a program that
    // uses DbgHelp itself takes too: a DLL's copy names nothing while the program holds the lock,
    // rather than call DbgHelp at the same time, and names its call once the lock is free.
    TEST(VtableReplace, EveryCopyOfTheRuntimeTakesTheDbgHelpLockOfTheProcess)
    {
        HANDLE lock = CreateSemaphoreW(nullptr, 1, 1, dbgHelpLockName().c_str());
        ASSERT_NE(lock, nullptr) << GetLastError();
        DllStandardError output(fs::path(testing::TempDir()) / "ct_runtime_library_lock.txt");
        HMODULE runtime = LoadLibraryA(CT_RUNTIME_LIBRARY);
        FARPROC entry = runtime ? GetProcAddress(runtime, "ct_runtime_library_trace") : nullptr;
        ASSERT_NE(entry, nullptr) << CT_RUNTIME_LIBRARY;
        auto* trace = reinterpret_cast<void (*)()>(entry);

        ASSERT_EQ(WaitForSingleObject(lock, INFINITE), WAIT_OBJECT_0);
        trace();
        (void)ReleaseSemaphore(lock, 1, nullptr);
        trace();
        FreeLibrary(runtime);
        CloseHandle(lock);
        const std::string boxes = output.contents();

        const size_t first = boxes.find("runtime_library:1");
        ASSERT_NE(first, std::string::npos) << boxes;
        EXPECT_TRUE(namesNothing(symbolOf(boxes, "runtime_library:1"))) << boxes;
        EXPECT_TRUE(
            names(symbolOf(boxes, "runtime_library:1", first + 1), "ct_runtime_library_measure"))
            << boxes;
    }

    // A DLL that links the runtime releases, when it unloads, what its copy opened for DbgHelp:
    // its handle of the lock, its session and the session's handles. Loaded, made to name a call
    // or only to name one from its destructors, and unloaded many times, it leaves the process no
    // more handles than after the first times.
    TEST(VtableReplace, ADllThatLinksTheRuntimeReleasesItsDbgHelpStateWhenUnloaded)
    {
        const auto cycle = [](bool trace)
        {
            HMODULE runtime = LoadLibraryA(CT_RUNTIME_LIBRARY);
            FARPROC entry = runtime ? GetProcAddress(runtime, "ct_runtime_library_trace") : nullptr;
            if (entry && trace)
            {
                reinterpret_cast<void (*)()>(entry)();
            }
            if (runtime)
            {
                FreeLibrary(runtime);
            }
            return entry != nullptr;
        };
        ASSERT_TRUE(cycle(true)) << CT_RUNTIME_LIBRARY;
        ASSERT_TRUE(cycle(false));
        DWORD before = 0;
        ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &before));
        constexpr int kCycles = 20;
        for (int index = 0; index < kCycles; ++index)
        {
            ASSERT_TRUE(cycle(index % 2 == 0));
        }
        DWORD after = 0;
        ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &after));
        // Kept, they would add two handles a cycle at least: the lock's and the session's.
        EXPECT_LT(after, before + kCycles)
            << "handles before the cycles: " << before << ", after: " << after;
    }

    // The slot of `module`'s import table that holds the address of `function`, imported from
    // `library`; null without one.
    void** importSlot(HMODULE module, const char* library, const char* function)
    {
        auto* base = reinterpret_cast<unsigned char*>(module);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& directory =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (directory.VirtualAddress == 0)
        {
            return nullptr;
        }
        for (auto* import =
                 reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
             import->Name != 0; ++import)
        {
            if (_stricmp(reinterpret_cast<const char*>(base + import->Name), library) != 0)
            {
                continue;
            }
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + import->OriginalFirstThunk);
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + import->FirstThunk);
            for (; names->u1.AddressOfData != 0; ++names, ++slots)
            {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                {
                    continue;
                }
                auto* by_name =
                    reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function) == 0)
                {
                    return reinterpret_cast<void**>(&slots->u1.Function);
                }
            }
        }
        return nullptr;
    }

    // Has the DLL that links the runtime call `replacement` in place of SymCleanup.
    bool replaceSymCleanup(HMODULE runtime, void* replacement, void** original = nullptr)
    {
        void** slot = importSlot(runtime, "dbghelp.dll", "SymCleanup");
        DWORD protection = 0;
        if (!slot || !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection))
        {
            return false;
        }
        if (original)
        {
            *original = *slot;
        }
        *slot = replacement;
        (void)VirtualProtect(slot, sizeof(*slot), protection, &protection);
        return true;
    }

    // In place of SymCleanup at process termination: ends the process with a code of its own.
    BOOL WINAPI symCleanupAtExit(HANDLE)
    {
        TerminateProcess(GetCurrentProcess(), 9);
        return TRUE;
    }

    // Loads a DLL that links the runtime, has its copy name a call, which starts its DbgHelp
    // session, then exits with the DLL still loaded. Exits with 0, or with 9 when the copy calls
    // SymCleanup as the process terminates.
    [[noreturn]] void exitWithARuntimeLibraryLoaded()
    {
        HMODULE runtime = LoadLibraryA(CT_RUNTIME_LIBRARY);
        FARPROC entry = runtime ? GetProcAddress(runtime, "ct_runtime_library_trace") : nullptr;
        if (!entry)
        {
            std::exit(2);
        }
        reinterpret_cast<void (*)()>(entry)();
        if (!replaceSymCleanup(runtime, reinterpret_cast<void*>(&symCleanupAtExit)))
        {
            std::exit(3);
        }
        std::exit(0);
    }

    // A DLL's copy of the runtime frees nothing while the process terminates: the guidance for
    // DllMain, which applies to the destructors that run from it, is to free resources only when
    // the DLL is unloaded dynamically, and not to call FreeLibrary then (#226).
    TEST(VtableReplaceDeathTest, ADllThatLinksTheRuntimeKeepsItsDbgHelpSessionAtProcessExit)
    {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
        EXPECT_EXIT(exitWithARuntimeLibraryLoaded(), testing::ExitedWithCode(0), "");
    }

    // Loads a DLL that links the runtime, has its copy name a call or not, and unloads it, when
    // its destructors name one; then loads and unloads another DLL. Exits with 0 when the
    // destructors' box named the DLL's function, or nothing, and the loader did not call into the
    // DLL unloaded; with 7 when the process has not ended within 20 seconds, as when the loader,
    // having called into the DLL unloaded, cannot let it exit.
    [[noreturn]] void unloadARuntimeLibraryThenLoadAnother(bool trace_before)
    {
        std::thread(
            []
            {
                Sleep(20000);
                TerminateProcess(GetCurrentProcess(), 7);
            })
            .detach();
        DllStandardError output(fs::path(testing::TempDir()) / "ct_runtime_library_unload.txt");
        HMODULE runtime = LoadLibraryA(CT_RUNTIME_LIBRARY);
        FARPROC entry = runtime ? GetProcAddress(runtime, "ct_runtime_library_trace") : nullptr;
        if (!entry)
        {
            std::exit(2);
        }
        if (trace_before)
        {
            reinterpret_cast<void (*)()>(entry)();
        }
        FreeLibrary(runtime);
        HMODULE loaded = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(entry), &loaded))
        {
            std::exit(3); // Still loaded: the test would prove nothing.
        }
        const std::string unloading = symbolOf(output.contents(), "runtime_library:2");
        if (!names(unloading, "ct_runtime_library_measure") && !namesNothing(unloading))
        {
            std::exit(6);
        }
        HMODULE other = LoadLibraryA(CT_RELOAD_LIBRARY_A);
        if (!other)
        {
            std::exit(4);
        }
        FreeLibrary(other);
        std::exit(0);
    }

    TEST(VtableReplaceDeathTest, ADllThatLinksTheRuntimeStopsWatchingTheLoaderWhenUnloaded)
    {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
        EXPECT_EXIT(unloadARuntimeLibraryThenLoadAnother(true), testing::ExitedWithCode(0), "");
    }

    // Its first virtual call comes from its destructors, which run once its watch has stopped.
    TEST(VtableReplaceDeathTest, ADllWhoseFirstVirtualCallComesAtUnloadStopsWatchingToo)
    {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
        EXPECT_EXIT(unloadARuntimeLibraryThenLoadAnother(false), testing::ExitedWithCode(0), "");
    }

    // The program's side of reload/ct_dllmain_library.cpp, whose constructor calls the hook
    // below with the loader lock held.
    HANDLE constructor_running = nullptr;
    ULONGLONG constructor_wait = 0;
    // A lookup that does not get DbgHelp waits 500 ms for it before it names nothing.
    constexpr ULONGLONG kWaitedInVain = 400;
} // namespace

// Lets another thread name a call, which finds a module loaded since it last named one, then
// names a call itself.
extern "C" __declspec(dllexport) void ct_test_constructor_hook(void* object, void* target)
{
    SetEvent(constructor_running);
    Sleep(300);
    const ULONGLONG start = GetTickCount64();
    __ct_vcall_trace(object, target, "dllmain:1", "<unknown>");
    constructor_wait = GetTickCount64() - start;
}

namespace
{
    // A thread names a call while another one, in a DLL's constructor, holds the loader lock and
    // then names one too. Exits with 0 when the constructor's box names the DLL's function, 10
    // when it names nothing after waiting for DbgHelp in vain, 11 when it names nothing without
    // having waited; 6 when the box names another function or is missing, 5 when the calls have
    // not ended within 20 seconds.
    [[noreturn]] void nameCallsWhileAConstructorHoldsTheLoaderLock()
    {
        constructor_running = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HMODULE named = LoadLibraryA(CT_RELOAD_LIBRARY_A);
        if (!constructor_running || !named)
        {
            std::exit(2);
        }
        void* object = callEntry(named, "ct_reload_object");
        void* target = callEntry(named, "ct_reload_target");
        // The session has started and listed the modules before the constructor runs.
        __ct_vcall_trace(object, target, "dllmain:0", "<unknown>");

        testing::internal::CaptureStderr();
        HANDLE ended = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread other(
            [&]
            {
                WaitForSingleObject(constructor_running, INFINITE);
                __ct_vcall_trace(object, target, "dllmain:2", "<unknown>");
            });
        std::thread loader(
            [&]
            {
                HMODULE library = LoadLibraryA(CT_DLLMAIN_LIBRARY);
                if (library)
                {
                    FreeLibrary(library);
                }
                SetEvent(constructor_running);
                SetEvent(ended);
            });
        if (WaitForSingleObject(ended, 20000) != WAIT_OBJECT_0)
        {
            TerminateProcess(GetCurrentProcess(), 5);
        }
        loader.join();
        other.join();
        const std::string symbol = symbolOf(testing::internal::GetCapturedStderr(), "dllmain:1");
        if (names(symbol, "ct_dllmain_measure"))
        {
            std::exit(0);
        }
        if (namesNothing(symbol))
        {
            std::exit(constructor_wait >= kWaitedInVain ? 10 : 11);
        }
        std::exit(6);
    }

    TEST(VtableReplaceDeathTest, ALookupUnderTheLoaderLockWaitsForNoOneForever)
    {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
        EXPECT_EXIT(
            nameCallsWhileAConstructorHoldsTheLoaderLock(),
            [](int status) { return status == 0 || status == 10 || status == 11; }, "");
    }
} // namespace
