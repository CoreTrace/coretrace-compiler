// SPDX-License-Identifier: Apache-2.0
// Constructors and destructors with priorities, written out of order: each kind runs in
// the order of its priorities, whatever its place in the file.
#include <stdio.h>

__attribute__((constructor(300))) static void construct_last(void)
{
    puts("constructor 300");
}

__attribute__((constructor(101))) static void construct_first(void)
{
    puts("constructor 101");
}

__attribute__((constructor(200))) static void construct_second(void)
{
    puts("constructor 200");
}

__attribute__((destructor(101))) static void destroy_101(void)
{
    puts("destructor 101");
}

__attribute__((destructor(300))) static void destroy_300(void)
{
    puts("destructor 300");
}

int main(void)
{
    puts("main");
    return 0;
}
