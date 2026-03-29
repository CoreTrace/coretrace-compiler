// SPDX-License-Identifier: Apache-2.0
#include <new>

int main()
{
    int* p = new (std::nothrow) int(42);
    new (std::nothrow) int(42);
    (void)p;
    return 0;
}
