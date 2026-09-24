// SPDX-License-Identifier: Apache-2.0
//
// ct_demangle reports whether a name was decorated by the platform's C++ ABI; the trace
// prints the decoded form next to the raw name only when it was, so a C function must
// not come back as "decoded".
#include "ct_runtime_helpers.h"

#include <gtest/gtest.h>

#include <string>

namespace
{
    TEST(Demangle, LeavesUndecoratedNamesAlone)
    {
        std::string out;
        EXPECT_FALSE(ct_demangle("main", out));
        EXPECT_FALSE(ct_demangle("plain_c", out));
        EXPECT_FALSE(ct_demangle("", out));
        EXPECT_FALSE(ct_demangle(nullptr, out));
    }

    // Names as clang emits them for `int compute(int)` on each platform.
    TEST(Demangle, DecodesThePlatformCppNames)
    {
        std::string out;
#if defined(_WIN32)
        ASSERT_TRUE(ct_demangle("?compute@@YAHH@Z", out));
        EXPECT_NE(out.find("compute(int)"), std::string::npos) << out;
#else
        ASSERT_TRUE(ct_demangle("_Z7computei", out));
        EXPECT_EQ(out, "compute(int)");
#endif
    }
} // namespace
