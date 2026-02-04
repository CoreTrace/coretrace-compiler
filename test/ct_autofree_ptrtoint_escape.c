#include <stdlib.h>
#include <stdint.h>

static volatile uintptr_t g_ptr_bits;

int main(void)
{
    void* p = malloc(24);
    g_ptr_bits = (uintptr_t)p; /* escape: stored globally */
    return g_ptr_bits == 0 ? 0 : 1;
}
