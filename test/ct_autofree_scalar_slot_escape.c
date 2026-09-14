// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>
#include <stdint.h>

static volatile uintptr_t g_ptr_bits;

static void publish(uintptr_t* slot)
{
    g_ptr_bits = *slot; /* escape: the integer slot is read through its address */
}

int main(void)
{
    void* p = malloc(24);
    uintptr_t x = (uintptr_t)p;
    publish(&x); /* the slot's address escapes to a callee */
    return g_ptr_bits == 0 ? 0 : 1;
}
