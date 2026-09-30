// SPDX-License-Identifier: Apache-2.0
// Blocks the conservative auto-free scan must keep, each referred to from one kind of
// root only: a list whose head is in a global and whose other nodes only the previous
// node refers to, a block a global points inside of, and a block on another thread's
// stack. And one it must release: the only pointer to it is in a block the program freed.
// The scan runs every few milliseconds while main sleeps, with its default budget.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct node
{
    struct node* next;
    int value;
};

struct holder
{
    char* block;
};

static struct node* list;
static char* inside;
static atomic_int done;

static struct node* push(struct node* next, int value)
{
    struct node* node = malloc(sizeof(*node));
    node->next = next;
    node->value = value;
    return node;
}

static void* hold(void* unused)
{
    (void)unused;
    char* volatile held = malloc(64);
    held[0] = 7;
    while (!atomic_load(&done))
    {
        usleep(1000);
    }
    const int intact = held[0] == 7;
    free(held);
    return (void*)(intptr_t)intact;
}

// Runs on a thread of its own, which exits before the scan: no stale copy of the pointer
// stays in its registers or on its stack.
static void* drop_reference(void* unused)
{
    (void)unused;
    struct holder* holder = malloc(sizeof(*holder));
    holder->block = malloc(32);
    free(holder);
    return NULL;
}

int main(void)
{
    list = push(push(push(NULL, 3), 2), 1);
    inside = (char*)malloc(64) + 8;
    inside[0] = 5;
    pthread_t dropper;
    pthread_t holder_thread;
    // The holder first: a thread created after the dropper exits could reuse its stack.
    if (pthread_create(&holder_thread, NULL, hold, NULL) != 0 ||
        pthread_create(&dropper, NULL, drop_reference, NULL) != 0 ||
        pthread_join(dropper, NULL) != 0)
    {
        return 2;
    }
    usleep(300 * 1000);
    atomic_store(&done, 1);
    void* intact = NULL;
    pthread_join(holder_thread, &intact);

    int sum = 0;
    while (list)
    {
        struct node* next = list->next;
        sum += list->value;
        free(list);
        list = next;
    }
    printf("sum=%d inside=%d held=%d\n", sum, inside[0], (int)(intptr_t)intact);
    free(inside - 8);
    return 0;
}
