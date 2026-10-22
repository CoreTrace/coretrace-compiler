// SPDX-License-Identifier: Apache-2.0
//
// On Windows, a virtual call box names its target through DbgHelp, which keeps the modules it
// has listed. A DLL replaced by another one at the same path, with the same time stamp, as
// reproducible builds give, must not keep the first one's names (#208): two builds of one
// library, their time stamps set to zero, are copied in turn to one path and loaded from there.
// The libraries are built from reload/ct_reload_library.cpp.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <windows.h>

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
        for (const Build& build : builds)
        {
            SCOPED_TRACE(build.name);
            ASSERT_TRUE(copyWithoutTimeStamp(build.source, path)) << path;
            HMODULE library = LoadLibraryW(path.c_str());
            ASSERT_NE(library, nullptr) << path;
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
} // namespace
