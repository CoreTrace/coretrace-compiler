#include <cstddef>
#include <new>

int main()
{
    int* p = new int(7);
    ::operator delete(p, sizeof(int));

    int* a = new int[4];
    ::operator delete[](a, sizeof(int) * 4);

#if defined(__cpp_aligned_new)
    auto* q = new (std::align_val_t(64)) int(1);
    ::operator delete(q, std::align_val_t(64));

    auto* r = new (std::align_val_t(64)) int[2];
    ::operator delete[](r, sizeof(int) * 2, std::align_val_t(64));
#endif

    return 0;
}
