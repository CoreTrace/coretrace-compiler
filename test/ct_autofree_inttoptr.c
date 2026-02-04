#include <stdlib.h>
#include <stdint.h>

int main(void)
{
    void* p = malloc(24);
    uintptr_t x = (uintptr_t)p;
    void* q = (void*)x;
    if (q)
    {
        (void)q;
    }
    return 0;
}
