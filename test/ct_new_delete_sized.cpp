// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <new>

int main()
{
    // Clang declares the sized forms by default from version 19 on; older ones only with
    // -fsized-deallocation.
#if defined(__cpp_sized_deallocation)
    int* p = new int(7);
    ::operator delete(p, sizeof(int));

    int* a = new int[4];
    ::operator delete[](a, sizeof(int) * 4);
#endif

#if defined(__cpp_aligned_new)
    auto* q = new (std::align_val_t(64)) int(1);
    ::operator delete(q, std::align_val_t(64));

#if defined(__cpp_sized_deallocation)
    auto* r = new (std::align_val_t(64)) int[2];
    ::operator delete[](r, sizeof(int) * 2, std::align_val_t(64));
#endif
#endif

    return 0;
}
