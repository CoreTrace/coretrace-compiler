// SPDX-License-Identifier: Apache-2.0
// A text buffer grown with realloc, split into words that point into it, sorted with
// qsort and deduplicated, then edited in place with memmove.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct word
{
    const char* start;
    size_t length;
};

static void* checked(void* ptr)
{
    if (!ptr)
    {
        exit(2);
    }
    return ptr;
}

static int compare_words(const void* lhs, const void* rhs)
{
    const struct word* a = lhs;
    const struct word* b = rhs;
    size_t common = a->length < b->length ? a->length : b->length;
    int order = memcmp(a->start, b->start, common);
    if (order != 0)
    {
        return order;
    }
    return (a->length > b->length) - (a->length < b->length);
}

int main(void)
{
    static const char* const phrases[] = {"the quick brown fox ", "jumps over ", "the lazy dog ",
                                          "and keeps running "};
    size_t capacity = 8;
    size_t length = 0;
    char* text = checked(malloc(capacity));
    for (int i = 0; i < 200; ++i)
    {
        const char* phrase = phrases[i % 4];
        size_t phrase_length = strlen(phrase);
        while (length + phrase_length + 1 > capacity)
        {
            capacity *= 2;
            text = checked(realloc(text, capacity));
        }
        memcpy(text + length, phrase, phrase_length);
        length += phrase_length;
    }
    text[length] = '\0';

    size_t word_capacity = 4;
    size_t word_count = 0;
    struct word* words = checked(malloc(word_capacity * sizeof(*words)));
    for (const char* p = text; *p;)
    {
        while (*p == ' ')
        {
            ++p;
        }
        if (!*p)
        {
            break;
        }
        const char* start = p;
        while (*p && *p != ' ')
        {
            ++p;
        }
        if (word_count == word_capacity)
        {
            word_capacity *= 2;
            words = checked(realloc(words, word_capacity * sizeof(*words)));
        }
        words[word_count].start = start;
        words[word_count].length = (size_t)(p - start);
        ++word_count;
    }
    qsort(words, word_count, sizeof(*words), compare_words);

    size_t distinct = 0;
    for (size_t i = 0; i < word_count; ++i)
    {
        if (distinct == 0 || compare_words(&words[distinct - 1], &words[i]) != 0)
        {
            words[distinct++] = words[i];
        }
    }
    printf("length=%zu capacity=%zu words=%zu distinct=%zu first=%.*s last=%.*s\n", length,
           capacity, word_count, distinct, (int)words[0].length, words[0].start,
           (int)words[distinct - 1].length, words[distinct - 1].start);

    size_t removed = 0;
    for (char* hit = strstr(text, "the "); hit; hit = strstr(hit, "the "))
    {
        memmove(hit, hit + 4, strlen(hit + 4) + 1);
        ++removed;
    }
    printf("removed=%zu remaining=%zu\n", removed, strlen(text));

    free(words);
    free(text);
    return 0;
}
