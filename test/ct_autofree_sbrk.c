#include <unistd.h>

int main(void)
{
    void* p = sbrk(64);
    if (p == (void*)-1)
    {
        return 0;
    }
    (void)p;
    return 0;
}
