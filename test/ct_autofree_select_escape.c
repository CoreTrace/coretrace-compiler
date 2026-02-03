#include <stdlib.h>
#include <stdint.h>

static volatile void* g_ptr;

int main(void)
{
    void* p = malloc(16);
    int cond = 1;
    void* r = cond ? p : NULL;

    g_ptr = r; /* escape: stored globally */

    return g_ptr == NULL ? 0 : 1;
}
