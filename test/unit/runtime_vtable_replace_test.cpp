// SPDX-License-Identifier: Apache-2.0
//
// On Windows, a virtual call box names its target through DbgHelp, which keeps the modules it
// has listed. A DLL replaced by another one at the same path, with the same time stamp, as
// reproducible builds give, must not keep the first one's names (#208): two builds of one
// library, their time stamps set to zero, are copied in turn to one path and loaded from there.
// The libraries are built from reload/ct_reload_library.cpp. A DLL loaded after DbgHelp listed
// the modules must be named too, after many unloads as well. The runtime uses DbgHelp in a session
// of its own.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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
} // namespace
