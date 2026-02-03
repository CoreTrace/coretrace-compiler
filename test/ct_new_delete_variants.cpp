#include <cstddef>
#include <new>

int main()
{
    int* p = new (std::nothrow) int(7);
    ::operator delete(p, std::nothrow);

    int* a = new (std::nothrow) int[4];
    ::operator delete[](a, std::nothrow);

#if defined(__cpp_lib_destroying_delete) && __cpp_lib_destroying_delete >= 201806L
    int* d = new int(1);
    ::operator delete(d, std::destroying_delete_t{});
#endif

    return 0;
}
