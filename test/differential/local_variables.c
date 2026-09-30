// SPDX-License-Identifier: Apache-2.0
// Blocks handed from one local variable to another before they leave the function that
// allocated them: a constructor that returns its block through a second variable, and a
// list built in a loop.
#include <stdio.h>
#include <stdlib.h>

struct point
{
    int x;
    int y;
};

struct node
{
    int value;
    struct node* next;
};

static struct point* point_new(int x, int y)
{
    struct point* point = malloc(sizeof(*point));
    if (!point)
    {
        exit(2);
    }
    point->x = x;
    point->y = y;
    struct point* result = point;
    return result;
}

static struct node* list_build(int length)
{
    struct node* head = NULL;
    for (int i = 0; i < length; ++i)
    {
        struct node* node = malloc(sizeof(*node));
        if (!node)
        {
            exit(2);
        }
        node->value = i;
        node->next = head;
        head = node;
    }
    return head;
}

int main(void)
{
    struct point* point = point_new(3, 4);
    printf("point=(%d,%d)\n", point->x, point->y);
    free(point);

    struct node* list = list_build(5);
    int sum = 0;
    while (list)
    {
        struct node* next = list->next;
        sum += list->value;
        free(list);
        list = next;
    }
    printf("sum=%d\n", sum);
    return 0;
}
