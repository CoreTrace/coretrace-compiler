// SPDX-License-Identifier: Apache-2.0
// Blocks the program keeps only through unusual references: a tagged pointer, an
// XOR-linked list, a pointer stored as an integer inside another block, a pointer held
// only by a global, and an interior pointer. Each block is read after the allocator has
// been churned, so a block released too early would hold other data.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct xor_node
{
    int value;
    // Address of the previous node XOR address of the next one.
    uintptr_t link;
};

struct holder
{
    uintptr_t hidden;
};

static int* global_block;

static void* checked(void* ptr)
{
    if (!ptr)
    {
        exit(2);
    }
    return ptr;
}

// malloc aligns blocks to at least 8 bytes, so bit 0 is free for the tag.
static uintptr_t make_tagged(int value)
{
    int* block = checked(malloc(sizeof(int)));
    *block = value;
    return (uintptr_t)block | 1u;
}

static int* untag(uintptr_t tagged)
{
    return (int*)(tagged & ~(uintptr_t)1);
}

static struct xor_node* make_xor_list(int count)
{
    struct xor_node* head = NULL;
    struct xor_node* last = NULL;
    for (int i = 0; i < count; ++i)
    {
        struct xor_node* node = checked(malloc(sizeof(*node)));
        node->value = i * 3 + 1;
        node->link = (uintptr_t)last;
        if (last)
        {
            last->link ^= (uintptr_t)node;
        }
        else
        {
            head = node;
        }
        last = node;
    }
    return head;
}

static long walk_xor_list(struct xor_node* head, int release)
{
    long sum = 0;
    uintptr_t previous = 0;
    struct xor_node* node = head;
    while (node)
    {
        sum += node->value;
        struct xor_node* next = (struct xor_node*)(node->link ^ previous);
        previous = (uintptr_t)node;
        if (release)
        {
            free(node);
        }
        node = next;
    }
    return sum;
}

int main(void)
{
    uintptr_t tagged = make_tagged(41);
    struct xor_node* list = make_xor_list(64);

    struct holder* holder = checked(malloc(sizeof(*holder)));
    int* numbers = checked(malloc(4 * sizeof(int)));
    for (int i = 0; i < 4; ++i)
    {
        numbers[i] = i * 10;
    }
    holder->hidden = (uintptr_t)numbers;

    global_block = checked(malloc(8 * sizeof(int)));
    for (int i = 0; i < 8; ++i)
    {
        global_block[i] = i * i;
    }

    int* middle = (int*)checked(malloc(16 * sizeof(int))) + 8;
    for (int i = -8; i < 8; ++i)
    {
        middle[i] = i + 100;
    }

    for (int i = 0; i < 1000; ++i)
    {
        void* scratch = checked(malloc(64));
        memset(scratch, 0xab, 64);
        free(scratch);
    }

    int tagged_value = *untag(tagged);
    long xor_sum = walk_xor_list(list, 0);
    int hidden_value = ((int*)holder->hidden)[3];
    printf("tagged=%d xor=%ld hidden=%d global=%d interior=%d,%d\n", tagged_value, xor_sum,
           hidden_value, global_block[7], middle[-8], middle[7]);

    free(untag(tagged));
    walk_xor_list(list, 1);
    free((int*)holder->hidden);
    free(holder);
    free(global_block);
    free(middle - 8);
    return 0;
}
