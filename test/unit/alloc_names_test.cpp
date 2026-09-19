// SPDX-License-Identifier: Apache-2.0
#include "compilerlib/instrumentation/alloc_internal.hpp"

#include <gtest/gtest.h>

#include <set>

namespace
{
    using compilerlib::EscapeState;
    using compilerlib::OperatorDeleteKind;
    using compilerlib::OperatorNewKind;

    TEST(EscapeLattice, RanksAreDistinctAndOrdered)
    {
        const EscapeState states[] = {
            EscapeState::Unreachable,  EscapeState::ReachableLocal, EscapeState::ReachableGlobal,
            EscapeState::EscapedStore, EscapeState::EscapedCall,    EscapeState::EscapedReturn,
            EscapeState::EscapedScan,
        };
        std::set<int> ranks;
        int previous = -1;
        for (EscapeState state : states)
        {
            const int rank = compilerlib::escapeRank(state);
            EXPECT_GT(rank, previous) << compilerlib::escapeStateName(state);
            ranks.insert(rank);
            previous = rank;
        }
        EXPECT_EQ(ranks.size(), std::size(states));
    }

    TEST(EscapeLattice, OnlyReachableLocalMayBeFreedAndEveryEscapeOutranksIt)
    {
        const int local = compilerlib::escapeRank(EscapeState::ReachableLocal);
        EXPECT_GT(compilerlib::escapeRank(EscapeState::ReachableGlobal), local);
        EXPECT_GT(compilerlib::escapeRank(EscapeState::EscapedStore), local);
        EXPECT_GT(compilerlib::escapeRank(EscapeState::EscapedCall), local);
        EXPECT_GT(compilerlib::escapeRank(EscapeState::EscapedReturn), local);
        EXPECT_GT(compilerlib::escapeRank(EscapeState::EscapedScan), local);
        EXPECT_LT(compilerlib::escapeRank(EscapeState::Unreachable), local);
    }

    TEST(EscapeLattice, NamesAreStable)
    {
        EXPECT_STREQ(compilerlib::escapeStateName(EscapeState::ReachableLocal), "REACHABLE_LOCAL");
        EXPECT_STREQ(compilerlib::escapeStateName(EscapeState::EscapedStore), "ESCAPED_STORE");
    }

    TEST(SymbolNames, AsmLabelPrefixIsStripped)
    {
        EXPECT_EQ(compilerlib::normalizeSymbolName("\x01_mmap"), "_mmap");
        EXPECT_EQ(compilerlib::normalizeSymbolName("mmap"), "mmap");
        EXPECT_EQ(compilerlib::normalizeSymbolName(""), "");
    }

    TEST(SymbolNames, MmapFamilyAcceptsDarwinVariants)
    {
        EXPECT_TRUE(compilerlib::isMmapLikeName("mmap"));
        EXPECT_TRUE(compilerlib::isMmapLikeName("\x01_mmap"));
        EXPECT_TRUE(compilerlib::isMmapLikeName("mmap$UNIX2003"));
        EXPECT_FALSE(compilerlib::isMmapLikeName("mmap64_wrapper"));
        EXPECT_TRUE(compilerlib::isMunmapLikeName("__munmap"));
        EXPECT_FALSE(compilerlib::isMunmapLikeName("mmap"));
        EXPECT_TRUE(compilerlib::isSbrkLikeName("sbrk"));
        EXPECT_TRUE(compilerlib::isBrkLikeName("_brk"));
        EXPECT_FALSE(compilerlib::isBrkLikeName("sbrk"));
    }

    TEST(OperatorNames, NewVariants)
    {
        bool isArray = true;
        OperatorNewKind kind = OperatorNewKind::Nothrow;
        ASSERT_TRUE(compilerlib::isOperatorNewName("_Znwm", isArray, kind));
        EXPECT_FALSE(isArray);
        EXPECT_EQ(kind, OperatorNewKind::Normal);

        ASSERT_TRUE(compilerlib::isOperatorNewName("_ZnamRKSt9nothrow_t", isArray, kind));
        EXPECT_TRUE(isArray);
        EXPECT_EQ(kind, OperatorNewKind::Nothrow);

        ASSERT_TRUE(compilerlib::isOperatorNewName("__Znam", isArray, kind));
        EXPECT_TRUE(isArray);

        EXPECT_FALSE(compilerlib::isOperatorNewName("malloc", isArray, kind));
        EXPECT_FALSE(compilerlib::isOperatorNewName("_ZnwmSt11align_val_t", isArray, kind));
    }

    TEST(OperatorNames, DeleteVariantsCoverSizedAlignedNothrowAndDestroying)
    {
        struct Case
        {
            const char* name;
            bool isArray;
            OperatorDeleteKind kind;
        };
        const Case cases[] = {
            {"_ZdlPv", false, OperatorDeleteKind::Normal},
            {"_ZdaPv", true, OperatorDeleteKind::Normal},
            {"_ZdlPvm", false, OperatorDeleteKind::Normal},
            {"_ZdaPvmSt11align_val_t", true, OperatorDeleteKind::Normal},
            {"_ZdlPvRKSt9nothrow_t", false, OperatorDeleteKind::Nothrow},
            {"_ZdaPvmSt11align_val_tRKSt9nothrow_t", true, OperatorDeleteKind::Nothrow},
            {"_ZdlPvSt19destroying_delete_t", false, OperatorDeleteKind::Destroying},
            {"__ZdaPvSt19destroying_delete_t", true, OperatorDeleteKind::Destroying},
        };
        for (const Case& c : cases)
        {
            bool isArray = !c.isArray;
            OperatorDeleteKind kind = OperatorDeleteKind::Nothrow;
            ASSERT_TRUE(compilerlib::isOperatorDeleteName(c.name, isArray, kind)) << c.name;
            EXPECT_EQ(isArray, c.isArray) << c.name;
            EXPECT_EQ(kind, c.kind) << c.name;
        }
        bool isArray = false;
        OperatorDeleteKind kind = OperatorDeleteKind::Normal;
        EXPECT_FALSE(compilerlib::isOperatorDeleteName("free", isArray, kind));
        EXPECT_FALSE(compilerlib::isOperatorDeleteName("_Znwm", isArray, kind));
    }

    TEST(FreeLikeNames, CoversRuntimeReleaseEntryPoints)
    {
        EXPECT_TRUE(compilerlib::isFreeLikeName("free"));
        EXPECT_TRUE(compilerlib::isFreeLikeName("__ct_free"));
        EXPECT_TRUE(compilerlib::isFreeLikeName("__ct_autofree_delete_array"));
        EXPECT_FALSE(compilerlib::isFreeLikeName("malloc"));
        EXPECT_FALSE(compilerlib::isFreeLikeName("__ct_malloc"));
    }
} // namespace
