// SPDX-License-Identifier: Apache-2.0
// container_of, written with integer arithmetic, applied to a pointer that is not a
// member of the containing type: the rebuilt base lies before the allocation, so reading
// the first field is an access before the block.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

struct node
{
    int key;
    int value;
};

#define container_of(ptr, type, member) ((type*)((uintptr_t)(ptr) - offsetof(type, member)))

int main(void)
{
    int* values = malloc(2 * sizeof(int));
    if (!values)
    {
        return 2;
    }
    values[0] = 1;
    values[1] = 2;

    struct node* wrong = container_of(&values[0], struct node, value);
    volatile int key = wrong->key;
    (void)key;

    free(values);
    return 0;
}
