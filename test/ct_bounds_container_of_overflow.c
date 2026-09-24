// SPDX-License-Identifier: Apache-2.0
// container_of, written with integer arithmetic, applied to the last element of an int
// array: the rebuilt base lies inside the block, and the second field of the node is
// past its end.
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

    struct node* wrong = container_of(&values[1], struct node, key);
    volatile int value = wrong->value;
    (void)value;

    free(values);
    return 0;
}
