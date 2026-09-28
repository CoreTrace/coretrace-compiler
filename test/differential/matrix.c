// SPDX-License-Identifier: Apache-2.0
// Matrices in a fixed-size local array, in a variable-length array and on the heap: a
// recursive determinant whose frames each hold a variable-length minor, a product
// computed through function pointers, and a struct returned by value.
#include <stdio.h>
#include <stdlib.h>

struct stats
{
    long sum;
    int min;
    int max;
};

typedef int (*combine_fn)(int, int);

static int add(int a, int b)
{
    return a + b;
}

static int multiply_values(int a, int b)
{
    return a * b;
}

static long determinant(int n, int m[n][n])
{
    if (n == 1)
    {
        return m[0][0];
    }
    long total = 0;
    int minor[n - 1][n - 1];
    for (int skip = 0; skip < n; ++skip)
    {
        for (int row = 1; row < n; ++row)
        {
            for (int column = 0, k = 0; column < n; ++column)
            {
                if (column != skip)
                {
                    minor[row - 1][k++] = m[row][column];
                }
            }
        }
        long sign = skip % 2 == 0 ? 1 : -1;
        total += sign * m[0][skip] * determinant(n - 1, minor);
    }
    return total;
}

static int* product(int n, const int* a, const int* b, combine_fn plus, combine_fn times)
{
    int* out = calloc((size_t)n * (size_t)n, sizeof(int));
    if (!out)
    {
        exit(2);
    }
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            for (int k = 0; k < n; ++k)
            {
                out[i * n + j] = plus(out[i * n + j], times(a[i * n + k], b[k * n + j]));
            }
        }
    }
    return out;
}

static struct stats summarize(int count, const int* values)
{
    struct stats stats = {0, values[0], values[0]};
    for (int i = 0; i < count; ++i)
    {
        stats.sum += values[i];
        stats.min = values[i] < stats.min ? values[i] : stats.min;
        stats.max = values[i] > stats.max ? values[i] : stats.max;
    }
    return stats;
}

int main(void)
{
    enum
    {
        kFixed = 5
    };
    int fixed[kFixed][kFixed];
    for (int row = 0; row < kFixed; ++row)
    {
        for (int column = 0; column < kFixed; ++column)
        {
            fixed[row][column] = (row * 7 + column * 3) % 11 - 5;
        }
    }

    volatile int size = 4;
    int n = size;
    int variable[n][n];
    for (int row = 0; row < n; ++row)
    {
        for (int column = 0; column < n; ++column)
        {
            variable[row][column] = row == column ? 2 : row - column;
        }
    }

    int* a = malloc((size_t)n * (size_t)n * sizeof(int));
    int* b = malloc((size_t)n * (size_t)n * sizeof(int));
    if (!a || !b)
    {
        return 2;
    }
    for (int i = 0; i < n * n; ++i)
    {
        a[i] = i % 5 - 2;
        b[i] = (i * 3) % 7 - 3;
    }
    int* result = product(n, a, b, add, multiply_values);
    struct stats stats = summarize(n * n, result);

    printf("det_fixed=%ld det_variable=%ld sum=%ld min=%d max=%d\n", determinant(kFixed, fixed),
           determinant(n, variable), stats.sum, stats.min, stats.max);

    free(result);
    free(b);
    free(a);
    return 0;
}
