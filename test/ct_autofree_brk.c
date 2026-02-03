#include <unistd.h>

int main(void)
{
    void* cur = sbrk(0);
    if (cur == (void*)-1)
    {
        return 0;
    }
    void* next = (char*)cur + 64;
    if (brk(next) != 0)
    {
        return 0;
    }
    (void)brk(cur);
    return 0;
}
