// SPDX-License-Identifier: Apache-2.0
// Singly linked list: insertion at both ends, removal through a pointer to the link,
// and in-place reversal.
#include <stdio.h>
#include <stdlib.h>

struct node
{
    int value;
    struct node* next;
};

static struct node* push_front(struct node* head, int value)
{
    struct node* node = malloc(sizeof(*node));
    if (!node)
    {
        exit(2);
    }
    node->value = value;
    node->next = head;
    return node;
}

static void push_back(struct node** head, int value)
{
    struct node** link = head;
    while (*link)
    {
        link = &(*link)->next;
    }
    *link = push_front(NULL, value);
}

static void remove_multiples(struct node** head, int divisor)
{
    struct node** link = head;
    while (*link)
    {
        struct node* node = *link;
        if (node->value % divisor == 0)
        {
            *link = node->next;
            free(node);
        }
        else
        {
            link = &node->next;
        }
    }
}

static struct node* reverse(struct node* head)
{
    struct node* reversed = NULL;
    while (head)
    {
        struct node* next = head->next;
        head->next = reversed;
        reversed = head;
        head = next;
    }
    return reversed;
}

int main(void)
{
    struct node* head = NULL;
    for (int i = 0; i < 100; ++i)
    {
        head = push_front(head, i);
    }
    for (int i = 100; i < 150; ++i)
    {
        push_back(&head, i);
    }
    remove_multiples(&head, 3);
    head = reverse(head);

    long weighted_sum = 0;
    int count = 0;
    for (const struct node* node = head; node; node = node->next)
    {
        ++count;
        weighted_sum += (long)node->value * count;
    }
    printf("count=%d weighted_sum=%ld first=%d\n", count, weighted_sum, head ? head->value : -1);

    while (head)
    {
        struct node* next = head->next;
        free(head);
        head = next;
    }
    return 0;
}
