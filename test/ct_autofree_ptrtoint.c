#include <stdlib.h>
#include <stdint.h>

int main(void)
{
    void* p = malloc(24);
    uintptr_t x = (uintptr_t)p;
    if ((x & 1u) == 0u)
    {
        (void)x;
    }
    return 0;
}
