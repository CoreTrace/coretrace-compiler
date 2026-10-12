// SPDX-License-Identifier: Apache-2.0
// One block of each C and C++ allocation API, resized when it can be, then released: the
// runtime traces each event in the same words on every system.
#include <cstdlib>

int main()
{
    void* volatile block = std::malloc(16);
    block = std::realloc(block, 64);
    std::free(block);

    int* volatile object = new int(1);
    delete object;

    int* volatile array = new int[4];
    delete[] array;
    return 0;
}
