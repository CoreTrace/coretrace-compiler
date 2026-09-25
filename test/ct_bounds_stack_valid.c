// SPDX-License-Identifier: Apache-2.0
// Valid accesses to stack objects, built to abort on any bounds error: indexed arrays,
// arrays passed to callees, struct fields, a valid container_of, memcpy and memset,
// recursion, early returns and a variable-length array.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define container_of(ptr, type, member) ((type*)((uintptr_t)(ptr) - offsetof(type, member)))

struct node
{
    int key;
    int value;
};

static int sum(const int* values, int count)
{
    int total = 0;
    for (int i = 0; i < count; ++i)
        total += values[i];
    return total;
}

static int depth_sum(int depth)
{
    int frame[8];
    for (int i = 0; i < 8; ++i)
        frame[i] = depth + i;
    if (depth == 0)
        return frame[7];
    return frame[depth % 8] + depth_sum(depth - 1);
}

static int first_positive(const int* values, int count)
{
    int copy[16];
    memcpy(copy, values, (size_t)count * sizeof(int));
    for (int i = 0; i < count; ++i)
        if (copy[i] > 0)
            return copy[i];
    return 0;
}

int main(void)
{
    int values[16];
    memset(values, 0, sizeof(values));
    for (int i = 0; i < 16; ++i)
        values[i] = i - 3;

    struct node node = {7, 8};
    struct node* back = container_of(&node.value, struct node, value);

    volatile int n = 5;
    int vla[n];
    for (int i = 0; i < n; ++i)
        vla[i] = i;

    volatile int total = sum(values, 16) + back->key + depth_sum(12) +
                         first_positive(values, 16) + vla[n - 1];
    (void)total;
    return 0;
}
