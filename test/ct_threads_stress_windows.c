// SPDX-License-Identifier: Apache-2.0
// The scenario of ct_threads_stress.c with Windows threads: several threads allocate,
// resize and release blocks at the same time, and check every byte they wrote. They share
// the runtime's allocation table: a record lost or mixed up between threads shows up as a
// false bounds error, corrupted data, a wrong leak count, a crash or a hang. Each thread
// leaks exactly one block, kept in a global so that auto-free leaves it alone.
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define THREADS 8
// Enough allocations for the table, which keeps the records of freed blocks, to grow past
// its initial 65,536 entries while the threads use it.
#define ROUNDS 20000
// Blocks a thread holds at once.
#define LIVE 64
#define MAX_SIZE 32

struct slot
{
    unsigned char* block;
    size_t size;
    unsigned char tag;
};

static void* volatile leaked[THREADS];

static unsigned next_random(unsigned* state)
{
    *state = *state * 1103515245u + 12345u;
    return *state >> 16;
}

static void fill(struct slot* slot, size_t from)
{
    for (size_t i = from; i < slot->size; ++i)
    {
        slot->block[i] = (unsigned char)(slot->tag + i);
    }
}

// Number of bytes among the first `size` that no longer hold what fill() wrote.
static long damaged(const struct slot* slot, size_t size)
{
    long count = 0;
    for (size_t i = 0; i < size; ++i)
    {
        count += slot->block[i] != (unsigned char)(slot->tag + i);
    }
    return count;
}

// Frees a slot's block, if any: the runtime warns about free(NULL).
static long release(struct slot* slot)
{
    if (!slot->block)
    {
        return 0;
    }
    const long errors = damaged(slot, slot->size);
    free(slot->block);
    slot->block = NULL;
    slot->size = 0;
    return errors;
}

static DWORD WINAPI worker(LPVOID arg)
{
    const unsigned index = (unsigned)(uintptr_t)arg;
    unsigned state = index + 1;
    struct slot slots[LIVE] = {{0}};
    long errors = 0;

    for (int round = 0; round < ROUNDS; ++round)
    {
        struct slot* slot = &slots[next_random(&state) % LIVE];
        const size_t size = 1 + next_random(&state) % MAX_SIZE;
        switch (next_random(&state) % 3)
        {
        case 0:
            errors += release(slot);
            slot->block = malloc(size);
            slot->size = size;
            slot->tag = (unsigned char)next_random(&state);
            fill(slot, 0);
            break;
        case 1:
        {
            // realloc keeps the bytes the old and new sizes have in common.
            const size_t kept = slot->size < size ? slot->size : size;
            slot->block = realloc(slot->block, size);
            errors += slot->block ? damaged(slot, kept) : 0;
            slot->size = size;
            fill(slot, kept);
            break;
        }
        default:
            errors += release(slot);
            break;
        }
    }

    for (int i = 0; i < LIVE; ++i)
    {
        errors += release(&slots[i]);
    }
    leaked[index] = malloc(16);
    return (DWORD)errors;
}

int main(void)
{
    HANDLE threads[THREADS];
    for (unsigned i = 0; i < THREADS; ++i)
    {
        threads[i] = CreateThread(NULL, 0, worker, (LPVOID)(uintptr_t)i, 0, NULL);
        if (threads[i] == NULL)
        {
            return 2;
        }
    }
    if (WaitForMultipleObjects(THREADS, threads, TRUE, INFINITE) != WAIT_OBJECT_0)
    {
        return 2;
    }

    long errors = 0;
    for (unsigned i = 0; i < THREADS; ++i)
    {
        DWORD result = 0;
        if (!GetExitCodeThread(threads[i], &result))
        {
            return 2;
        }
        errors += (long)result;
        CloseHandle(threads[i]);
    }
    printf("threads=%d rounds=%d damaged=%ld\n", THREADS, ROUNDS, errors);
    return errors == 0 ? 0 : 1;
}
