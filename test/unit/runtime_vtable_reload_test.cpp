// SPDX-License-Identifier: Apache-2.0
//
// The vtable boxes name the types and functions of the libraries loaded when they are
// printed: a library is loaded, unloaded, replaced by another build with other names, then
// loaded again, and each box names what is loaded at that moment, whatever the names
// decoded before. The libraries are built from reload/ct_reload_library.cpp.
#include <gtest/gtest.h>

#include <sstream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern "C"
{
    void __ct_vtable_dump(void* this_ptr, const char* site, const char* static_type);
    void __ct_vcall_trace(void* this_ptr, void* target, const char* site, const char* static_type);
}

namespace
{
    // A library loaded for the life of the object.
    class Library
    {
      public:
        explicit Library(const char* path)
        {
#if defined(_WIN32)
            handle_ = LoadLibraryA(path);
#else
            handle_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
            EXPECT_NE(handle_, nullptr) << path;
        }
        ~Library()
        {
            if (!handle_)
            {
                return;
            }
#if defined(_WIN32)
            FreeLibrary(static_cast<HMODULE>(handle_));
#else
            dlclose(handle_);
#endif
        }
        Library(const Library&) = delete;
        Library& operator=(const Library&) = delete;

        void* call(const char* function) const
        {
            if (!handle_)
            {
                return nullptr;
            }
#if defined(_WIN32)
            auto* entry = reinterpret_cast<void* (*)()>(
                GetProcAddress(static_cast<HMODULE>(handle_), function));
#else
            auto* entry = reinterpret_cast<void* (*)()>(dlsym(handle_, function));
#endif
            EXPECT_NE(entry, nullptr) << function;
            return entry ? entry() : nullptr;
        }

      private:
        void* handle_ = nullptr;
    };

    TEST(VtableReload, BoxesNameTheTypesOfTheLibraryLoadedNow)
    {
        struct Build
        {
            const char* path;
            const char* name;
            const char* other;
        };
        const Build builds[] = {{CT_RELOAD_LIBRARY_A, "reload_a", "reload_b"},
                                {CT_RELOAD_LIBRARY_B, "reload_b", "reload_a"},
                                {CT_RELOAD_LIBRARY_A, "reload_a", "reload_b"}};
        for (const Build& build : builds)
        {
            SCOPED_TRACE(build.name);
            std::string boxes;
            {
                Library library(build.path);
                void* object = library.call("ct_reload_object");
                void* target = library.call("ct_reload_target");
                ASSERT_NE(object, nullptr);
                ASSERT_NE(target, nullptr);

                testing::internal::CaptureStderr();
                __ct_vtable_dump(object, "reload:1", "<unknown>");
                __ct_vcall_trace(object, target, "reload:2", "<unknown>");
                boxes = testing::internal::GetCapturedStderr();
            }

            // The type line of each box names the type of the library loaded now: decoded,
            // reload_a::Widget, or as the Windows runtime prints an RTTI name it does not
            // decode, .?AUWidget@reload_a@@.
            size_t types = 0;
            std::istringstream lines(boxes);
            for (std::string line; std::getline(lines, line);)
            {
                if (line.find(" type ") != std::string::npos)
                {
                    ++types;
                    EXPECT_NE(line.find(build.name), std::string::npos) << line;
                }
            }
            EXPECT_EQ(types, 2u) << boxes;
            EXPECT_EQ(boxes.find(build.other), std::string::npos) << boxes;
#if !defined(_WIN32)
            // The Windows runtime names a target from debug information the test libraries
            // do not carry.
            EXPECT_NE(boxes.find(std::string(build.name) + "::describe(int)"), std::string::npos)
                << boxes;
#endif
        }
    }
} // namespace
