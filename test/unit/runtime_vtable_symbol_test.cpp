// SPDX-License-Identifier: Apache-2.0
//
// The symbol line of a virtual call box names the target as dladdr does. On Linux, the runtime
// reads the symbol from the module's tables itself, without the loader's lock that dladdr takes
// (#223). This program exports its functions. On Linux, its code and its read-only data are in
// segments apart, not adjacent in memory, as with 64 KiB pages and as the link options make
// them here: _dl_find_object then gives each segment of the main program apart. POSIX only.
// The libraries are built from reload/ct_reload_library.cpp, one of them on Linux with a
// DT_HASH table only.
#include "ct_runtime_internal.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <dlfcn.h>
#if defined(__linux__)
#include <link.h>
#include <sys/auxv.h>
#include <unistd.h>
#endif

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

#if defined(__linux__)
    // The main program's segments in memory.
    int listProgramSegments(dl_phdr_info* info, size_t, void* data)
    {
        auto* segments = static_cast<std::vector<std::pair<uintptr_t, uintptr_t>>*>(data);
        for (int index = 0; index < info->dlpi_phnum; ++index)
        {
            const ElfW(Phdr)& segment = info->dlpi_phdr[index];
            if (segment.p_type == PT_LOAD)
            {
                const uintptr_t start = info->dlpi_addr + segment.p_vaddr;
                segments->push_back({start, start + segment.p_memsz});
            }
        }
        return 1;
    }

    // The case that NamesATargetOfTheProgramAsDladdrDoes checks on Linux: the program's code
    // and its dynamic symbols in segments apart, which _dl_find_object gives apart.
    TEST_F(VtableSymbol, TheProgramsCodeIsApartFromItsSymbols)
    {
        std::vector<std::pair<uintptr_t, uintptr_t>> segments;
        dl_iterate_phdr(listProgramSegments, &segments);
        // Apart as glibc counts them: one that does not start on the page after the previous
        // one ends (elf/rtld.c).
        const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
        bool apart = false;
        for (size_t index = 1; index < segments.size(); ++index)
        {
            apart = apart || (segments[index].first & ~(page - 1)) !=
                                 ((segments[index - 1].second + page - 1) & ~(page - 1));
        }
        EXPECT_TRUE(apart);

        link_map* program = nullptr;
        ASSERT_EQ(dlinfo(dlopen(nullptr, RTLD_NOW), RTLD_DI_LINKMAP, &program), 0) << dlerror();
        uintptr_t symbols = 0;
        for (const ElfW(Dyn)* entry = program->l_ld; entry->d_tag != DT_NULL; ++entry)
        {
            symbols = entry->d_tag == DT_SYMTAB ? entry->d_un.d_ptr : symbols;
        }
        const auto segmentOf = [&](uintptr_t address)
        {
            for (size_t index = 0; index < segments.size(); ++index)
            {
                if (address >= segments[index].first && address < segments[index].second)
                {
                    return static_cast<int>(index);
                }
            }
            return -1;
        };
        const int code = segmentOf(reinterpret_cast<uintptr_t>(&ct_symbol_program_target));
        ASSERT_NE(code, -1);
        EXPECT_NE(segmentOf(symbols), code);
    }

    // A library without a GNU hash table, whose symbols the runtime finds through DT_HASH.
    TEST_F(VtableSymbol, NamesATargetOfALibraryWithoutAGnuHashTableAsDladdrDoes)
    {
        void* library = dlopen(CT_RELOAD_LIBRARY_SYSV, RTLD_NOW | RTLD_LOCAL);
        ASSERT_NE(library, nullptr) << dlerror();
        link_map* map = nullptr;
        ASSERT_EQ(dlinfo(library, RTLD_DI_LINKMAP, &map), 0) << dlerror();
        bool hash = false;
        bool gnu_hash = false;
        for (const ElfW(Dyn)* entry = map->l_ld; entry->d_tag != DT_NULL; ++entry)
        {
            hash = hash || entry->d_tag == DT_HASH;
            gnu_hash = gnu_hash || entry->d_tag == DT_GNU_HASH;
        }
        ASSERT_TRUE(hash && !gnu_hash);
        dlclose(library);
        expectLibraryNamedAsDladdrNamesIt(CT_RELOAD_LIBRARY_SYSV);
    }

    // The vDSO's dynamic section is read-only: its tables are where it says once the vDSO's
    // base is added, unlike those of the other modules, which the loader relocated.
    TEST_F(VtableSymbol, NamesAddressesOfTheVdsoAsDladdrDoes)
    {
        const auto* header = reinterpret_cast<const ElfW(Ehdr)*>(getauxval(AT_SYSINFO_EHDR));
        if (!header)
        {
            GTEST_SKIP() << "no vDSO";
        }
        const auto* segments = reinterpret_cast<const ElfW(Phdr)*>(
            reinterpret_cast<const char*>(header) + header->e_phoff);
        uintptr_t base = 0;
        uintptr_t start = 0;
        uintptr_t end = 0;
        for (int index = 0; index < header->e_phnum; ++index)
        {
            if (segments[index].p_type == PT_LOAD && base == 0)
            {
                base = reinterpret_cast<uintptr_t>(header) - segments[index].p_vaddr;
            }
            if (segments[index].p_type == PT_LOAD && (segments[index].p_flags & PF_X) != 0)
            {
                start = base + segments[index].p_vaddr;
                end = start + segments[index].p_memsz;
            }
        }
        ASSERT_LT(start, end);
        int named = 0;
        for (uintptr_t address = start; address < end; address += 16)
        {
            void* target = reinterpret_cast<void*>(address);
            named += dladdrName(target) != "<unknown>";
            EXPECT_TRUE(namedAsDladdrNamesIt(target));
        }
        EXPECT_GT(named, 0);
    }
#endif
} // namespace
