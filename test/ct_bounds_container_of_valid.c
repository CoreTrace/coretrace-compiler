// SPDX-License-Identifier: Apache-2.0
// A correct container_of, written with integer arithmetic: the rebuilt node is the
// allocated one, so no access may be reported (the fixture is built to abort on any
// bounds error).
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
    struct node* node = malloc(sizeof *node);
    if (!node)
    {
        return 2;
    }
    node->key = 1;
    node->value = 2;

    int* member = &node->value;
    struct node* back = container_of(member, struct node, value);
    int ok = back->key == 1 && back->value == 2;

    free(node);
    return ok ? 0 : 1;
}
