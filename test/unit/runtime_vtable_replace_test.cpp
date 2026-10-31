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
// one forever. The crash handler's backtraces, which name frames through the same session, name
// none after a DLL unloaded since (#225). The copy in a DLL frees the names it decoded when the DLL
// unloads: the DLL keeps no more heap blocks than one without the runtime. The boxes of the DLL's
// destructors still name types then, as they do when the process exits (#227).
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
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

    // A busy block of one of the process's heaps.
    struct HeapBlock
    {
        HANDLE heap;
        const unsigned char* address;
        size_t size;

        bool operator<(const HeapBlock& other) const
        {
            return std::tie(heap, address, size) < std::tie(other.heap, other.address, other.size);
        }
    };

    // The busy blocks of every heap of the process, sorted, into `blocks`, whose storage is
    // reserved beforehand, so that the walk does not change the heaps it walks. False when that
    // storage is too small.
    bool listBusyBlocks(std::vector<HeapBlock>& blocks)
    {
        blocks.clear();
        HANDLE heaps[256];
        const DWORD count = std::min<DWORD>(GetProcessHeaps(256, heaps), 256);
        for (DWORD index = 0; index < count; ++index)
        {
            if (!HeapLock(heaps[index]))
            {
                continue;
            }
            PROCESS_HEAP_ENTRY entry{};
            bool room = true;
            while (room && HeapWalk(heaps[index], &entry))
            {
                if ((entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) == 0)
                {
                    continue;
                }
                room = blocks.size() < blocks.capacity();
                if (room)
                {
                    blocks.push_back({heaps[index], static_cast<const unsigned char*>(entry.lpData),
                                      entry.cbData});
                }
            }
            HeapUnlock(heaps[index]);
            if (!room)
            {
                return false;
            }
        }
        std::sort(blocks.begin(), blocks.end());
        return true;
    }

    constexpr int kLoadCycles = 20;

    // Loads `library` and unloads it; a DLL that links the runtime names a call first when `trace`
    // is set, and from its destructors when it unloads.
    bool loadAndUnload(const char* library, bool trace)
    {
        HMODULE module = LoadLibraryA(library);
        if (!module)
        {
            return false;
        }
        FARPROC entry = GetProcAddress(module, "ct_runtime_library_trace");
        if (entry && trace)
        {
            reinterpret_cast<void (*)()>(entry)();
        }
        FreeLibrary(module);
        return true;
    }

    // The blocks of the process's heaps that kLoadCycles loads and unloads of `library` leave
    // allocated, after two to warm up, into `kept`; a call is named before every other unload.
    bool blocksKeptAcrossLoads(const char* library, std::vector<HeapBlock>& kept)
    {
        std::vector<HeapBlock> before;
        std::vector<HeapBlock> after;
        before.reserve(1 << 18);
        after.reserve(1 << 18);
        if (!loadAndUnload(library, true) || !loadAndUnload(library, false) ||
            !listBusyBlocks(before))
        {
            return false;
        }
        for (int index = 0; index < kLoadCycles; ++index)
        {
            if (!loadAndUnload(library, index % 2 == 0))
            {
                return false;
            }
        }
        if (!listBusyBlocks(after))
        {
            return false;
        }
        kept.clear();
        std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                            std::back_inserter(kept));
        return true;
    }

    // The blocks by size, then the text that some of them start with.
    std::string describe(const std::vector<HeapBlock>& blocks)
    {
        std::map<size_t, size_t> sizes;
        for (const HeapBlock& block : blocks)
        {
            ++sizes[block.size];
        }
        std::ostringstream out;
        out << blocks.size() << " blocks kept:";
        for (const auto& [size, count] : sizes)
        {
            out << " " << count << " of " << size << " bytes;";
        }
        size_t shown = 0;
        for (const HeapBlock& block : blocks)
        {
            size_t length = 0;
            while (length < block.size && length < 64 && block.address[length] >= 0x20 &&
                   block.address[length] < 0x7f)
            {
                ++length;
            }
            if (length >= 8 && shown++ < 16)
            {
                out << "\n  " << std::string(reinterpret_cast<const char*>(block.address), length);
            }
        }
        return out.str();
    }

    // Whether `block` holds the name of the type or of the function that the boxes of
    // reload/ct_runtime_library.cpp name, as the strings of a cache of names do: small blocks,
    // where a debug C runtime puts its header before the text.
    bool holdsALibraryName(const HeapBlock& block)
    {
        constexpr size_t kSmallBlock = 1024;
        if (block.size > kSmallBlock)
        {
            return false;
        }
        const std::string_view bytes(reinterpret_cast<const char*>(block.address), block.size);
        for (const char* name : {".?AUGadget@", "ct_runtime_library_measure"})
        {
            if (bytes.find(name) != std::string_view::npos)
            {
                return true;
            }
        }
        return false;
    }

    // A DLL that links the runtime frees, when FreeLibrary unloads it, the names its copy decoded
    // for its boxes (#227). Loaded, made to name a call or only to name one from its destructors,
    // and unloaded many times, it leaves in the process's heaps no block that holds the name of
    // its type or of its function.
    TEST(VtableReplace, ADllThatLinksTheRuntimeFreesTheNamesItDecodedWhenUnloaded)
    {
        DllStandardError output(fs::path(testing::TempDir()) / "ct_runtime_library_names.txt");
        std::vector<HeapBlock> kept;
        ASSERT_TRUE(blocksKeptAcrossLoads(CT_RUNTIME_LIBRARY, kept)) << CT_RUNTIME_LIBRARY;
        EXPECT_EQ(std::count_if(kept.begin(), kept.end(), holdsALibraryName), 0) << describe(kept);
    }

    // Nor does it keep any other block across the loads: no more than a DLL built alike that does
    // not link the runtime.
    TEST(VtableReplace, ADllThatLinksTheRuntimeKeepsNoMoreHeapBlocksThanOneWithout)
    {
        DllStandardError output(fs::path(testing::TempDir()) / "ct_runtime_library_blocks.txt");
        std::vector<HeapBlock> control;
        ASSERT_TRUE(blocksKeptAcrossLoads(CT_RELOAD_LIBRARY_A, control)) << CT_RELOAD_LIBRARY_A;
        std::vector<HeapBlock> kept;
        ASSERT_TRUE(blocksKeptAcrossLoads(CT_RUNTIME_LIBRARY, kept)) << CT_RUNTIME_LIBRARY;
        // Kept at each load, a block would add kLoadCycles of them.
        EXPECT_LT(kept.size(), control.size() + kLoadCycles)
            << describe(kept) << "\nwithout the runtime: " << describe(control);
    }

    // The line `label` of the first box of `site`, empty without one.
    std::string lineOf(const std::string& boxes, const char* site, const char* label)
    {
        const size_t box = boxes.find(site);
        const size_t line = box == std::string::npos ? box : boxes.find(label, box);
        if (line == std::string::npos)
        {
            return std::string();
        }
        return boxes.substr(line, boxes.find('\n', line) - line);
    }

    // The boxes that the DLL's destructors print when FreeLibrary unloads it still name the
    // dynamic type of their object, whether the copy decoded that name before or not.
    TEST(VtableReplace, TheBoxesOfADllsDestructorsNameTheirTypeWhenItUnloads)
    {
        for (const bool trace_before : {true, false})
        {
            DllStandardError output(fs::path(testing::TempDir()) / "ct_runtime_library_types.txt");
            HMODULE runtime = LoadLibraryA(CT_RUNTIME_LIBRARY);
            FARPROC entry = runtime ? GetProcAddress(runtime, "ct_runtime_library_trace") : nullptr;
            ASSERT_NE(entry, nullptr) << CT_RUNTIME_LIBRARY;
            if (trace_before)
            {
                reinterpret_cast<void (*)()>(entry)();
            }
            FreeLibrary(runtime);
            const std::string boxes = output.contents();
            EXPECT_TRUE(names(lineOf(boxes, "runtime_library:2", "type"), "Gadget"))
                << "named a call before: " << trace_before << "\n"
                << boxes;
        }
    }

    // Loads a DLL that links the runtime, has its copy name a call, then exits with the DLL
    // loaded: the DLL's destructors name a call as the process terminates, into the file at
    // `path`.
    [[noreturn]] void exitWithTheBoxesOfARuntimeLibraryIn(const fs::path& path)
    {
        // Never destroyed: the DLL writes to the file until the process ends.
        DllStandardError output(path);
        HMODULE runtime = LoadLibraryA(CT_RUNTIME_LIBRARY);
        FARPROC entry = runtime ? GetProcAddress(runtime, "ct_runtime_library_trace") : nullptr;
        if (!entry)
        {
            std::exit(2);
        }
        reinterpret_cast<void (*)()>(entry)();
        std::exit(0);
    }

    // As they do when the process exits: the copy in a DLL keeps its names until the end.
    TEST(VtableReplaceDeathTest, TheBoxesOfADllsDestructorsNameTheirTypeAtProcessExit)
    {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
        const fs::path path = fs::path(testing::TempDir()) / "ct_runtime_library_exit.txt";
        std::error_code ignored;
        fs::remove(path, ignored);
        EXPECT_EXIT(exitWithTheBoxesOfARuntimeLibraryIn(path), testing::ExitedWithCode(0), "");
        std::ifstream in(path, std::ios::binary);
        const std::string boxes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        EXPECT_TRUE(names(lineOf(boxes, "runtime_library:2", "type"), "Gadget")) << boxes;
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

    std::atomic<int> sym_cleanups{0};
    BOOL(WINAPI* real_sym_cleanup)(HANDLE) = nullptr;

    BOOL WINAPI countingSymCleanup(HANDLE process)
    {
        sym_cleanups.fetch_add(1);
        return real_sym_cleanup(process);
    }

    // FreeLibrary still has the DLL's copy close its session.
    TEST(VtableReplace, ADllThatLinksTheRuntimeClosesItsDbgHelpSessionWhenUnloaded)
    {
        HMODULE runtime = LoadLibraryA(CT_RUNTIME_LIBRARY);
        FARPROC entry = runtime ? GetProcAddress(runtime, "ct_runtime_library_trace") : nullptr;
        ASSERT_NE(entry, nullptr) << CT_RUNTIME_LIBRARY;
        reinterpret_cast<void (*)()>(entry)();
        void* original = nullptr;
        ASSERT_TRUE(
            replaceSymCleanup(runtime, reinterpret_cast<void*>(&countingSymCleanup), &original));
        real_sym_cleanup = reinterpret_cast<BOOL(WINAPI*)(HANDLE)>(original);
        sym_cleanups.store(0);
        FreeLibrary(runtime);
        EXPECT_EQ(sym_cleanups.load(), 1);
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

// The TLS callback of the runtime's watch of the loader, which stops the watch when the module
// that holds this copy of the runtime detaches.
extern "C" const PIMAGE_TLS_CALLBACK ct_module_changes_tls_callback;

namespace
{
    // The crash handler names the frames of its backtraces through the same session (#225). The
    // runtime reads CT_BACKTRACE when the process starts: this sets it for the processes started
    // meanwhile, a death test's child among them, whose runtime then installs the handler.
    class BacktraceInChildren
    {
      public:
        BacktraceInChildren()
        {
            wchar_t value[256];
            const DWORD length = GetEnvironmentVariableW(L"CT_BACKTRACE", value, 256);
            had_value_ = length != 0 && length < 256;
            if (had_value_)
            {
                previous_.assign(value, length);
            }
            SetEnvironmentVariableW(L"CT_BACKTRACE", L"1");
        }
        ~BacktraceInChildren()
        {
            SetEnvironmentVariableW(L"CT_BACKTRACE", had_value_ ? previous_.c_str() : nullptr);
        }
        BacktraceInChildren(const BacktraceInChildren&) = delete;
        BacktraceInChildren& operator=(const BacktraceInChildren&) = delete;

      private:
        bool had_value_ = false;
        std::wstring previous_;
    };

    struct CrashFrame
    {
        unsigned long long address = 0;
        std::string name;
    };

    // The frames of a crash backtrace in a standard error: the lines "  at <address> [<name>]".
    std::vector<CrashFrame> crashFramesOf(const std::string& output)
    {
        std::vector<CrashFrame> frames;
        std::istringstream lines(output);
        for (std::string line; std::getline(lines, line);)
        {
            const size_t at = line.find("  at ");
            if (at == std::string::npos)
            {
                continue;
            }
            char* end = nullptr;
            CrashFrame frame;
            frame.address = std::strtoull(line.c_str() + at + 5, &end, 16);
            frame.name = end;
            frame.name.erase(0, frame.name.find_first_not_of(' '));
            frame.name.erase(frame.name.find_last_not_of("\r\n ") + 1);
            frames.push_back(std::move(frame));
        }
        return frames;
    }

    // An image that the child printed after a mark: its base and size.
    struct CrashImage
    {
        unsigned long long base = 0;
        unsigned long long size = 0;

        bool holds(unsigned long long address) const
        {
            return address - base < size;
        }
    };

    CrashImage imageAfter(const std::string& output, const char* mark)
    {
        CrashImage image;
        const size_t at = output.find(mark);
        if (at != std::string::npos)
        {
            char* end = nullptr;
            image.base = std::strtoull(output.c_str() + at + std::strlen(mark), &end, 16);
            image.size = std::strtoull(end, nullptr, 16);
        }
        return image;
    }

    // What the backtrace of a process that the crash handler ended shows.
    enum class Backtrace
    {
        // A name for the frame of RaiseException, whose module the loader kept loaded all along.
        NamesTheModulesLoadedAllAlong,
        // That name, and a frame in the second build of the library, whose image the child
        // printed after "fault:2", while no frame is named after the first build, which the
        // loader unloaded from that base.
        NamesNothingAfterTheFirstBuild,
        // Frames, none of them named.
        NamesNothing,
        // Names for frames of the executable, whose image the child printed after "exe:2", and
        // for no other frame.
        NamesTheExecutableOnly,
    };

    class CrashBacktrace : public testing::MatcherInterface<const std::string&>
    {
      public:
        explicit CrashBacktrace(Backtrace expected) : expected_(expected) {}

        bool MatchAndExplain(const std::string& output,
                             testing::MatchResultListener* listener) const override
        {
            const std::vector<CrashFrame> frames = crashFramesOf(output);
            if (output.find("ct: fatal exception code=") == std::string::npos || frames.empty())
            {
                *listener << "the crash handler printed no backtrace";
                return false;
            }
            const auto named = [&](const char* name)
            {
                return std::find_if(frames.begin(), frames.end(), [&](const CrashFrame& frame)
                                    { return frame.name.find(name) != std::string::npos; });
            };
            if (expected_ == Backtrace::NamesNothing)
            {
                const auto any =
                    std::find_if(frames.begin(), frames.end(),
                                 [](const CrashFrame& frame) { return !frame.name.empty(); });
                if (any != frames.end())
                {
                    *listener << "a frame is named " << any->name;
                    return false;
                }
                return true;
            }
            if (expected_ == Backtrace::NamesTheExecutableOnly)
            {
                const CrashImage executable = imageAfter(output, "exe:2 ");
                const auto outside = std::find_if(
                    frames.begin(), frames.end(), [&](const CrashFrame& frame)
                    { return !frame.name.empty() && !executable.holds(frame.address); });
                if (outside != frames.end())
                {
                    *listener << "a frame outside the executable is named " << outside->name;
                    return false;
                }
                if (std::none_of(frames.begin(), frames.end(),
                                 [&](const CrashFrame& frame) {
                                     return !frame.name.empty() && executable.holds(frame.address);
                                 }))
                {
                    *listener << "no frame of the executable is named";
                    return false;
                }
                return true;
            }
            if (std::none_of(frames.begin(), frames.end(), [](const CrashFrame& frame)
                             { return frame.name == "RaiseException"; }))
            {
                *listener << "no frame is named RaiseException";
                return false;
            }
            if (expected_ == Backtrace::NamesTheModulesLoadedAllAlong)
            {
                return true;
            }
            if (const auto stale = named("reload_a"); stale != frames.end())
            {
                *listener << "a frame is named after the first build: " << stale->name;
                return false;
            }
            const CrashImage second = imageAfter(output, "fault:2 ");
            if (std::none_of(frames.begin(), frames.end(),
                             [&](const CrashFrame& frame) { return second.holds(frame.address); }))
            {
                *listener << "no frame is in the second build";
                return false;
            }
            return true;
        }

        void DescribeTo(std::ostream* os) const override
        {
            switch (expected_)
            {
            case Backtrace::NamesTheModulesLoadedAllAlong:
                *os << "is a crash backtrace that names RaiseException";
                break;
            case Backtrace::NamesNothingAfterTheFirstBuild:
                *os << "is a crash backtrace that names RaiseException, has a frame in the second "
                       "build, and names none after the first";
                break;
            case Backtrace::NamesNothing:
                *os << "is a crash backtrace that names no frame";
                break;
            case Backtrace::NamesTheExecutableOnly:
                *os << "is a crash backtrace that names frames of the executable only";
                break;
            }
        }

      private:
        Backtrace expected_;
    };

    testing::Matcher<const std::string&> crashBacktrace(Backtrace expected)
    {
        return testing::MakeMatcher(new CrashBacktrace(expected));
    }

    // The code of the exceptions that the faults raise, here and in the reload library's fault().
    // A process that the crash handler ends after the fault exits with it; the children exit with
    // 2 when they cannot set their fault up, with 7 when the crash handler has not ended the
    // process in time, and with 8 when the fault returned.
    constexpr DWORD kFaultCode = 0xE0000225;

    testing::ExitedWithCode endedByTheFault()
    {
        return testing::ExitedWithCode(static_cast<int>(kFaultCode));
    }

    // Raises an exception that no handler catches, on a thread of its own: on the test's thread,
    // the test's own handler would catch it. Ends with 7 when the crash handler has not ended
    // the process within 20 seconds.
    [[noreturn]] void faultOnAThread(void (*fault)())
    {
        std::thread(
            []
            {
                Sleep(20000);
                TerminateProcess(GetCurrentProcess(), 7);
            })
            .detach();
        std::thread(fault).join();
        std::exit(8);
    }

    void raiseUnhandledException()
    {
        RaiseException(kFaultCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }

    // Without a virtual call box, the session's modules date from the handler's installation:
    // the frames of the modules it listed then, which the loader kept loaded, keep their names.
    TEST(BacktraceReplaceDeathTest, ACrashNamesTheFramesOfTheModulesLoadedAllAlong)
    {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
        BacktraceInChildren backtrace;
        EXPECT_EXIT(faultOnAThread(&raiseUnhandledException), endedByTheFault(),
                    crashBacktrace(Backtrace::NamesTheModulesLoadedAllAlong));
    }

    // Prints `mark`, then the base and the size of the image of `module`, for the matcher.
    void printImage(const char* mark, HMODULE module)
    {
        const auto* image = reinterpret_cast<const unsigned char*>(module);
        const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
        std::fprintf(stderr, "%s %p %lx\n", mark, static_cast<const void*>(image),
                     static_cast<unsigned long>(headers->OptionalHeader.SizeOfImage));
        std::fflush(stderr);
    }

    // The copy of the library that the child loads, built in turn from both builds.
    fs::path faultingLibraryPath()
    {
        return fs::path(testing::TempDir()) / "ct_faulting_library.dll";
    }

    // Loads the first build of the library at a path and names its function, so that the
    // session lists it with its names, unloads it, then loads the second build from the same
    // path at the first one's base, and faults in it. Prints why and exits with 2 when it cannot
    // set this up.
    [[noreturn]] void faultInALibraryReplacedAtItsBase()
    {
        const fs::path path = faultingLibraryPath();
        HMODULE first =
            copyWithoutTimeStamp(CT_RELOAD_LIBRARY_A, path) ? LoadLibraryW(path.c_str()) : nullptr;
        if (!first ||
            !names(symbolOf(traceACall(first, "fault:1"), "fault:1"), "reload_a::describe"))
        {
            std::fprintf(stderr, "fault: the first build was not named\n");
            std::exit(2);
        }
        FreeLibrary(first);
        HMODULE replaced =
            copyWithoutTimeStamp(CT_RELOAD_LIBRARY_B, path) ? LoadLibraryW(path.c_str()) : nullptr;
        auto* fault = reinterpret_cast<void (*)()>(
            replaced == first ? callEntry(replaced, "ct_reload_fault") : nullptr);
        if (!fault)
        {
            // Elsewhere, the session would not list the first build at its address.
            std::fprintf(stderr, "fault: the second build is not at the first one's base\n");
            std::exit(2);
        }
        printImage("fault:2", replaced);
        faultOnAThread(fault);
    }

    TEST(BacktraceReplaceDeathTest, ACrashNamesNoFrameAfterADllUnloadedFromItsBase)
    {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
        BacktraceInChildren backtrace;
        EXPECT_EXIT(faultInALibraryReplacedAtItsBase(), endedByTheFault(),
                    crashBacktrace(Backtrace::NamesNothingAfterTheFirstBuild));
        std::error_code ignored;
        fs::remove(faultingLibraryPath(), ignored);
    }

    // Stops the watch of the loader, as the TLS callback does when the module that holds this
    // copy of the runtime detaches, then faults: the loader no longer reports what it unloads.
    [[noreturn]] void faultOnceTheWatchHasStopped()
    {
        ct_module_changes_tls_callback(nullptr, DLL_PROCESS_DETACH, nullptr);
        faultOnAThread(&raiseUnhandledException);
    }

    TEST(BacktraceReplaceDeathTest, ACrashOnceTheWatchHasStoppedNamesNoFrame)
    {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
        BacktraceInChildren backtrace;
        EXPECT_EXIT(faultOnceTheWatchHasStopped(), endedByTheFault(),
                    crashBacktrace(Backtrace::NamesNothing));
    }

    // Has the loader unload twice as many DLLs as the runtime keeps the bases of since the
    // handler's installation, then faults. Prints why and exits with 2 when the library does
    // not load.
    [[noreturn]] void faultAfterManyUnloads()
    {
        constexpr int kUnloads = 2 * 64;
        for (int unload = 0; unload < kUnloads; ++unload)
        {
            HMODULE library = LoadLibraryA(CT_RELOAD_LIBRARY_A);
            if (!library)
            {
                std::fprintf(stderr, "unloads: the library did not load\n");
                std::exit(2);
            }
            FreeLibrary(library);
        }
        printImage("exe:2", GetModuleHandleW(nullptr));
        faultOnAThread(&raiseUnhandledException);
    }

    // The runtime can no longer tell which of its session's modules the loader unloaded: only
    // the frames of the executable, which the loader cannot have unloaded, keep their names.
    TEST(BacktraceReplaceDeathTest, ACrashAfterManyUnloadsNamesTheExecutableOnly)
    {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
        BacktraceInChildren backtrace;
        EXPECT_EXIT(faultAfterManyUnloads(), endedByTheFault(),
                    crashBacktrace(Backtrace::NamesTheExecutableOnly));
    }
} // namespace
