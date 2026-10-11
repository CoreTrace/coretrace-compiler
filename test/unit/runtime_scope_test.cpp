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
} // namespace
