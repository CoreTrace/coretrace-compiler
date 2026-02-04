#include <stdlib.h>

int main(void)
{
    void* p = aligned_alloc(64, 256);
    (void)p;
    return 0;
}
