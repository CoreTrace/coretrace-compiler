// SPDX-License-Identifier: Apache-2.0
// container_of applied to an array that is not inside a struct node: the rebuilt
// pointer starts before the array, and reading its first field reads before it.
#include <stddef.h>
#include <stdint.h>

#define container_of(ptr, type, member) ((type*)((uintptr_t)(ptr) - offsetof(type, member)))

struct node
{
    int key;
    int value;
};

int main(void)
{
    int values[2] = {1, 2};
    struct node* wrong = container_of(&values[0], struct node, value);
    int key = wrong->key;
    (void)key;
    return 0;
}
