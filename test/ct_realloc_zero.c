#include <stdlib.h>

int main(void)
{
    char *p = (char *)malloc(32);
    p = (char *)realloc(p, 0); // free-like behavior per libc
    (void)p;
    return 0;
}
