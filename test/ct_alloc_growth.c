#include <stdlib.h>
#include <stdio.h>

int main(void) {
    const size_t count = 70000; // > 2^16 to trigger table growth
    void **ptrs = (void **)calloc(count, sizeof(void *));
    if (!ptrs) {
        printf("alloc for ptrs failed\n");
        return 1;
    }

    size_t i = 0;
    for (; i < count; ++i) {
        ptrs[i] = malloc(16);
        if (!ptrs[i]) {
            printf("malloc failed at %zu\n", i);
            break;
        }
    }

    for (size_t j = 0; j < i; ++j) {
        free(ptrs[j]);
    }
    free(ptrs);
    return 0;
}
