// SPDX-License-Identifier: Apache-2.0
//
// The symbol line of a virtual call box names the target as dladdr does. On Linux, the runtime
// reads the symbol from the module's tables itself, without the loader's lock that dladdr takes
// (#223). This program exports its functions. POSIX only. The library is built from
// reload/ct_reload_library.cpp.
#include "ct_runtime_internal.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <dlfcn.h>

extern "C"
{
    void __ct_vcall_trace(void* this_ptr, void* target, const char* site, const char* static_type);

    // A function of the main program that its dynamic symbols hold.
    __attribute__((visibility("default"), noinline)) int ct_symbol_program_target(int value)
    {
        return value + 1;
    }
}

namespace
{
    // An object of the test program that no dynamic symbol holds.
    int unnamed_object = 0;

    struct Local
    {
        virtual ~Local() = default;
        virtual int value() const
        {
            return 3;
        }
    };

    Local local;

    // The line of the box that has the label.
    std::string lineOf(const std::string& box, const char* label)
    {
        const size_t at = box.find(label);
        if (at == std::string::npos)
        {
            return std::string();
        }
        const size_t start = box.rfind('\n', at);
        const size_t end = box.find('\n', at);
        return box.substr(start == std::string::npos ? 0 : start + 1,
                          end == std::string::npos ? std::string::npos : end - start - 1);
    }

    // The name dladdr gives the symbol that holds `target`, or "<unknown>".
    std::string dladdrName(const void* target)
    {
        Dl_info info;
        return dladdr(target, &info) != 0 && info.dli_sname ? info.dli_sname : "<unknown>";
    }

    // The symbol line of the box of a call to `target`.
    std::string symbolLine(void* target)
    {
        testing::internal::CaptureStderr();
        __ct_vcall_trace(&local, target, "named:1", "<unknown>");
        return lineOf(testing::internal::GetCapturedStderr(), "symbol");
    }

    // Whether the box names `target` as dladdr does.
    testing::AssertionResult namedAsDladdrNamesIt(void* target)
    {
        const std::string expected = dladdrName(target);
        const std::string symbol = symbolLine(target);
        if (symbol.find(" " + expected + " ") != std::string::npos)
        {
            return testing::AssertionSuccess();
        }
        return testing::AssertionFailure()
               << target << ": dladdr names " << expected << ", the box: " << symbol;
    }

    class VtableSymbol : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            saved_ = ct_get_features();
            ct_set_enabled(CT_FEATURE_VTABLE_DIAG, 1);
        }
        void TearDown() override
        {
            ct_set_enabled(CT_FEATURE_VTABLE_DIAG, (saved_ & CT_FEATURE_VTABLE_DIAG) != 0);
        }

      private:
        uint64_t saved_ = 0;
    };

    // The functions and objects of a library: a function, an address inside it, and an object
    // that no dynamic symbol holds.
    void expectLibraryNamedAsDladdrNamesIt(const char* path)
    {
        void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        ASSERT_NE(library, nullptr) << dlerror();
        void* target = dlsym(library, "ct_reload_target");
        void* object = dlsym(library, "ct_reload_object");
        ASSERT_NE(target, nullptr) << dlerror();
        ASSERT_NE(object, nullptr) << dlerror();
        using Entry = void* (*)();
        void* describe = reinterpret_cast<Entry>(target)();
        for (void* named : {describe, target, static_cast<void*>(static_cast<char*>(describe) + 1)})
        {
            ASSERT_NE(dladdrName(named), "<unknown>");
            EXPECT_TRUE(namedAsDladdrNamesIt(named));
        }
        EXPECT_TRUE(namedAsDladdrNamesIt(reinterpret_cast<Entry>(object)()));
        dlclose(library);
    }

    TEST_F(VtableSymbol, NamesATargetOfALibraryAsDladdrDoes)
    {
        expectLibraryNamedAsDladdrNamesIt(CT_RELOAD_LIBRARY_A);
    }

    TEST_F(VtableSymbol, NamesATargetOfTheCLibraryAsDladdrDoes)
    {
        void* puts_address = dlsym(RTLD_DEFAULT, "puts");
        ASSERT_NE(puts_address, nullptr) << dlerror();
        for (void* named : {puts_address, static_cast<void*>(static_cast<char*>(puts_address) + 1)})
        {
            ASSERT_NE(dladdrName(named), "<unknown>");
            EXPECT_TRUE(namedAsDladdrNamesIt(named));
        }
    }

    TEST_F(VtableSymbol, NamesATargetOfTheProgramAsDladdrDoes)
    {
        void* target = reinterpret_cast<void*>(&ct_symbol_program_target);
        ASSERT_EQ(dladdrName(target), "ct_symbol_program_target");
        EXPECT_TRUE(namedAsDladdrNamesIt(target));
        EXPECT_TRUE(namedAsDladdrNamesIt(&unnamed_object));
    }
} // namespace
