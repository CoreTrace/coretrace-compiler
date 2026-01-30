#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

int main(void)
{
    const size_t page_size = 4096;
    const size_t pages = 70000; // adjust down if memory is tight
    const size_t size = pages * page_size;

    char* buf = (char*)malloc(size);
    if (!buf)
    {
        printf("malloc failed\n");
        return 1;
    }

    for (size_t i = 0; i < size; i += page_size)
    {
        buf[i] = (char)(i / page_size);
    }

    free(buf);
    return 0;
}
