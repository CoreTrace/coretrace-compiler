// SPDX-License-Identifier: Apache-2.0
//
// With the vtable diagnostics, a virtual call box warns of a module mismatch when the vtable
// and the function the call reaches belong to different modules. Two modules match when their
// paths, basenames, install names, frameworks or canonical paths (realpath()) are the same.
// These tests compare modules: the same one, distinct ones, one loaded through a link that
// changes target, one whose file is gone, and, on Linux, two loaded copies of one file that
// only their canonical paths tell are the same. POSIX only: the Windows runtime compares
// modules otherwise. The libraries are built from reload/ct_reload_library.cpp.
#include "ct_runtime_internal.h"

#include <gtest/gtest.h>

#include <climits>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <dlfcn.h>

extern "C"
{
    void __ct_vcall_trace(void* this_ptr, void* target, const char* site, const char* static_type);
}

namespace
{
    namespace fs = std::filesystem;

    // A library loaded for the life of the object, in the default namespace or, on Linux, in a
    // new one.
    class Library
    {
      public:
        explicit Library(const fs::path& path, bool new_namespace = false)
        {
#if defined(__linux__)
            handle_ = new_namespace ? dlmopen(LM_ID_NEWLM, path.c_str(), RTLD_NOW | RTLD_LOCAL)
                                    : dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#else
            (void)new_namespace;
            handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
            EXPECT_NE(handle_, nullptr) << path << ": " << dlerror();
        }
        ~Library()
        {
            if (handle_)
            {
                dlclose(handle_);
            }
        }
        Library(const Library&) = delete;
        Library& operator=(const Library&) = delete;

        void* object() const
        {
            return call("ct_reload_object");
        }
        void* target() const
        {
            return call("ct_reload_target");
        }

      private:
        void* call(const char* function) const
        {
            auto* entry =
                handle_ ? reinterpret_cast<void* (*)()>(dlsym(handle_, function)) : nullptr;
            EXPECT_NE(entry, nullptr) << function;
            return entry ? entry() : nullptr;
        }

        void* handle_ = nullptr;
    };

    class VtableModules : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            saved_features_ = ct_get_features();
            ct_set_enabled(CT_FEATURE_VTABLE_DIAG, 1);
            std::string pattern = (fs::temp_directory_path() / "ct_vtable_modules_XXXXXX").string();
            const char* made = mkdtemp(pattern.data());
            ASSERT_NE(made, nullptr);
            scratch_ = made;
        }
        void TearDown() override
        {
            ct_set_enabled(CT_FEATURE_VTABLE_DIAG, (saved_features_ & CT_FEATURE_VTABLE_DIAG) != 0);
            std::error_code ignored;
            fs::remove_all(scratch_, ignored);
        }

        // The box of a virtual call on `object` that reaches `target`.
        static std::string vcallBox(void* object, void* target)
        {
            testing::internal::CaptureStderr();
            __ct_vcall_trace(object, target, "modules:1", "<unknown>");
            return testing::internal::GetCapturedStderr();
        }

        static bool warnsOfAMismatch(const std::string& box)
        {
            return box.find("module mismatch") != std::string::npos;
        }

        // A symbolic link at `name` in the scratch directory to `target`, replacing the one there.
        fs::path link(const std::string& name, const fs::path& target) const
        {
            const fs::path path = scratch_ / name;
            std::error_code ignored;
            fs::remove(path, ignored);
            fs::create_symlink(target, path);
            return path;
        }

        fs::path scratch_;

      private:
        uint64_t saved_features_ = 0;
    };

    TEST_F(VtableModules, AVtableAndATargetOfOneModuleMatch)
    {
        Library library(CT_RELOAD_LIBRARY_A);
        const std::string box = vcallBox(library.object(), library.target());
        EXPECT_NE(box.find("vmod"), std::string::npos) << box;
        EXPECT_NE(box.find("tmod"), std::string::npos) << box;
        EXPECT_FALSE(warnsOfAMismatch(box)) << box;
    }

    TEST_F(VtableModules, AVtableAndATargetOfDistinctModulesDoNotMatch)
    {
        Library a(CT_RELOAD_LIBRARY_A);
        Library b(CT_RELOAD_LIBRARY_B);
        const std::string box = vcallBox(a.object(), b.target());
        EXPECT_TRUE(warnsOfAMismatch(box)) << box;
    }

    // Loaded through a link, unloaded, the link pointed at the other build, loaded again: each
    // time, the module loaded through the link matches itself and not the other build.
    TEST_F(VtableModules, AModuleLoadedThroughALinkThatChangesTarget)
    {
        const struct
        {
            const char* through_link;
            const char* other;
        } loads[] = {{CT_RELOAD_LIBRARY_A, CT_RELOAD_LIBRARY_B},
                     {CT_RELOAD_LIBRARY_B, CT_RELOAD_LIBRARY_A}};
        for (const auto& load : loads)
        {
            SCOPED_TRACE(load.through_link);
            const fs::path path = link("current.so", load.through_link);
            Library linked(path);
            Library other(load.other);
            EXPECT_FALSE(warnsOfAMismatch(vcallBox(linked.object(), linked.target())));
            EXPECT_TRUE(warnsOfAMismatch(vcallBox(linked.object(), other.target())));
        }
    }

    // A loaded library whose file is gone: realpath() fails for it, and the other criteria
    // decide as before.
    TEST_F(VtableModules, AModuleWhoseFileIsGoneIsComparedOtherwise)
    {
        const fs::path copy = scratch_ / "copy.so";
        fs::copy_file(CT_RELOAD_LIBRARY_A, copy);
        Library copied(copy);
        Library b(CT_RELOAD_LIBRARY_B);
        fs::remove(copy);
        char resolved[PATH_MAX];
        ASSERT_EQ(realpath(copy.c_str(), resolved), nullptr);

        const std::string same = vcallBox(copied.object(), copied.target());
        EXPECT_NE(same.find("vmod"), std::string::npos) << same;
        EXPECT_FALSE(warnsOfAMismatch(same)) << same;
        EXPECT_TRUE(warnsOfAMismatch(vcallBox(copied.object(), b.target())));
    }

#if defined(__linux__)
    // One file loaded twice, under an alias in a namespace of its own and under its own path:
    // the two copies differ in path and basename, and only their canonical paths match.
    TEST_F(VtableModules, TwoCopiesOfOneFileMatchByTheirCanonicalPaths)
    {
        const fs::path alias = link("alias.so", CT_RELOAD_LIBRARY_A);
        Library aliased(alias, true);
        Library direct(CT_RELOAD_LIBRARY_A);
        ASSERT_NE(aliased.object(), direct.object()) << "the file was not loaded twice";

        const std::string box = vcallBox(aliased.object(), direct.target());
        EXPECT_NE(box.find("alias.so"), std::string::npos) << box;
        EXPECT_FALSE(warnsOfAMismatch(box)) << box;
    }
#endif
} // namespace
