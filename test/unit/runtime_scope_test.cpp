// SPDX-License-Identifier: Apache-2.0
//
// The scope of runtime work on a thread (CtRuntimeScope), inside which the Windows runtime
// does not track the allocations it makes for itself (#197).
#include "ct_runtime_internal.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <thread>

namespace
{
    TEST(RuntimeScope, Nests)
    {
        ASSERT_FALSE(ct_in_runtime_scope());
        {
            CtRuntimeScope outer;
            EXPECT_TRUE(ct_in_runtime_scope());
            {
                CtRuntimeScope inner;
                EXPECT_EQ(ct_runtime_depth, 2u);
            }
            EXPECT_EQ(ct_runtime_depth, 1u);
        }
        EXPECT_FALSE(ct_in_runtime_scope());
    }

    TEST(RuntimeScope, EndsWhenAnExceptionLeavesIt)
    {
        try
        {
            CtRuntimeScope scope;
            throw std::runtime_error("leaves the scope");
        }
        catch (const std::runtime_error&)
        {
        }
        EXPECT_FALSE(ct_in_runtime_scope());
    }

    TEST(RuntimeScope, IsPerThread)
    {
        CtRuntimeScope scope;
        bool other_thread_in_scope = true;
        std::thread([&] { other_thread_in_scope = ct_in_runtime_scope(); }).join();
        EXPECT_FALSE(other_thread_in_scope);
        EXPECT_TRUE(ct_in_runtime_scope());
    }

#if defined(_WIN32)
    // A block allocated inside a scope is the runtime's: not in the table. The same
    // allocation outside one is the program's, and tracked.
    TEST(RuntimeScope, KeepsTheRuntimesOwnAllocationsUntracked)
    {
        unsigned char state = CT_ENTRY_EMPTY;
        void* own = nullptr;
        {
            CtRuntimeScope scope;
            own = __ct_malloc(32, "test:runtime");
            ASSERT_NE(own, nullptr);
            EXPECT_EQ(ct_table_lookup(own, nullptr, nullptr, nullptr, &state), 0);
            __ct_free(own, "test:runtime");
        }

        void* program = __ct_malloc(32, "test:program");
        ASSERT_NE(program, nullptr);
        ASSERT_EQ(ct_table_lookup(program, nullptr, nullptr, nullptr, &state), 1);
        EXPECT_EQ(state, CT_ENTRY_USED);
        __ct_free(program, "test:program");
    }
#endif
} // namespace
