// SPDX-License-Identifier: Apache-2.0
// Error handling with setjmp and longjmp: a parser leaves several frames that hold local
// arrays without returning, then the program calls deeper frames with fresh local arrays
// on the same stack.
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static jmp_buf on_error;
static char bad_character;
static char last_label[16];

static int parse_digit(char c)
{
    char label[16];
    snprintf(label, sizeof(label), "digit %c", c);
    memcpy(last_label, label, sizeof(label));
    if (c < '0' || c > '9')
    {
        bad_character = c;
        longjmp(on_error, 1);
    }
    return c - '0';
}

static int parse_number(const char* text)
{
    int digits[8];
    int count = 0;
    for (; *text && count < 8; ++text)
    {
        digits[count++] = parse_digit(*text);
    }
    int value = 0;
    for (int i = 0; i < count; ++i)
    {
        value = value * 10 + digits[i];
    }
    return value;
}

static int sum_numbers(const char* const* texts, int count)
{
    int partial[4] = {0, 0, 0, 0};
    for (int i = 0; i < count; ++i)
    {
        partial[i % 4] += parse_number(texts[i]);
    }
    return partial[0] + partial[1] + partial[2] + partial[3];
}

static long fill_and_sum(int depth)
{
    long frame[32];
    for (int i = 0; i < 32; ++i)
    {
        frame[i] = (long)depth * i;
    }
    long total = 0;
    for (int i = 0; i < 32; ++i)
    {
        total += frame[i];
    }
    return depth == 0 ? total : total + fill_and_sum(depth - 1);
}

int main(void)
{
    static const char* const inputs[][3] = {{"12", "345", "6789"}, {"12", "3x4", "56"}};
    int results[2];
    for (int round = 0; round < 2; ++round)
    {
        if (setjmp(on_error) == 0)
        {
            results[round] = sum_numbers(inputs[round], 3);
        }
        else
        {
            results[round] = -bad_character;
        }
    }
    printf("first=%d second=%d last='%s' stack=%ld\n", results[0], results[1], last_label,
           fill_and_sum(20));
    return 0;
}
