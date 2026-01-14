#include <stdlib.h>

int main(void) {
    char *p = (char *)malloc(8);
    if (p) {
        p[0] = 'a';
    }
    free(p);

    char *q = (char *)calloc(4, 4);
    q = (char *)realloc(q, 64);
    free(q);

    malloc(16); // unreachable -> tracing-malloc-unreachable (+ autofree if enabled)
    return 0;
}
