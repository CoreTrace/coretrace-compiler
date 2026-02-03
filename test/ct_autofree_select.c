#include <stdlib.h>

int main(void)
{
    void* p = malloc(16);
    void* q = malloc(32);
    int cond = 1;
    void* r = cond ? p : q;
    (void)r;
    return 0;
}
