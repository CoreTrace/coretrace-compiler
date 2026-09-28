// SPDX-License-Identifier: Apache-2.0
// Chained hash table: keys copied with strdup (allocated inside libc, released by the
// program), buckets rehashed into a larger array, lookups and deletions.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct entry
{
    char* key;
    int value;
    struct entry* next;
};

struct table
{
    struct entry** buckets;
    size_t bucket_count;
    size_t size;
};

static void* checked(void* ptr)
{
    if (!ptr)
    {
        exit(2);
    }
    return ptr;
}

static size_t hash(const char* key)
{
    size_t hash = 5381;
    for (const unsigned char* p = (const unsigned char*)key; *p; ++p)
    {
        hash = hash * 33 + *p;
    }
    return hash;
}

static void grow(struct table* table)
{
    size_t count = table->bucket_count * 2;
    struct entry** buckets = checked(calloc(count, sizeof(*buckets)));
    for (size_t i = 0; i < table->bucket_count; ++i)
    {
        struct entry* entry = table->buckets[i];
        while (entry)
        {
            struct entry* next = entry->next;
            size_t index = hash(entry->key) % count;
            entry->next = buckets[index];
            buckets[index] = entry;
            entry = next;
        }
    }
    free(table->buckets);
    table->buckets = buckets;
    table->bucket_count = count;
}

static void put(struct table* table, const char* key, int value)
{
    size_t index = hash(key) % table->bucket_count;
    for (struct entry* entry = table->buckets[index]; entry; entry = entry->next)
    {
        if (strcmp(entry->key, key) == 0)
        {
            entry->value = value;
            return;
        }
    }
    struct entry* entry = checked(malloc(sizeof(*entry)));
    entry->key = checked(strdup(key));
    entry->value = value;
    entry->next = table->buckets[index];
    table->buckets[index] = entry;
    if (++table->size > table->bucket_count * 3 / 4)
    {
        grow(table);
    }
}

static const struct entry* find(const struct table* table, const char* key)
{
    const struct entry* entry = table->buckets[hash(key) % table->bucket_count];
    while (entry && strcmp(entry->key, key) != 0)
    {
        entry = entry->next;
    }
    return entry;
}

static int erase(struct table* table, const char* key)
{
    for (struct entry** link = &table->buckets[hash(key) % table->bucket_count]; *link;
         link = &(*link)->next)
    {
        if (strcmp((*link)->key, key) == 0)
        {
            struct entry* entry = *link;
            *link = entry->next;
            free(entry->key);
            free(entry);
            --table->size;
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    struct table table = {checked(calloc(4, sizeof(struct entry*))), 4, 0};
    char key[32];
    for (int i = 0; i < 500; ++i)
    {
        snprintf(key, sizeof(key), "key-%d", i * 7 % 311);
        put(&table, key, i);
    }
    int erased = 0;
    for (int i = 0; i < 311; i += 5)
    {
        snprintf(key, sizeof(key), "key-%d", i);
        erased += erase(&table, key);
    }
    long checksum = 0;
    for (int i = 0; i < 311; ++i)
    {
        snprintf(key, sizeof(key), "key-%d", i);
        const struct entry* entry = find(&table, key);
        if (entry)
        {
            checksum += (long)entry->value * i;
        }
    }
    printf("size=%zu buckets=%zu erased=%d checksum=%ld\n", table.size, table.bucket_count, erased,
           checksum);

    for (size_t i = 0; i < table.bucket_count; ++i)
    {
        struct entry* entry = table.buckets[i];
        while (entry)
        {
            struct entry* next = entry->next;
            free(entry->key);
            free(entry);
            entry = next;
        }
    }
    free(table.buckets);
    return 0;
}
