#include <new>

int main()
{
    int* p = new (std::nothrow) int(42);
    new (std::nothrow) int(42);
    (void)p;
    return 0;
}
