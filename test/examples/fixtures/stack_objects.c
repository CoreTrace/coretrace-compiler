// SPDX-License-Identifier: Apache-2.0
// The stack objects a function registers for bounds checks: those whose address
// escapes or that an access reaches at an offset not known at compile time. The
// others are only accessed at constant offsets inside them.
struct pair
{
    int first;
    int second;
};

int consume(int* values);

int objects(int index)
{
    int scalar = index;
    struct pair pair = {1, 2};
    int indexed[4] = {0};  // registered: dynamic index
    int escaping[4] = {0}; // registered: passed to a call
    indexed[index & 3] = scalar;
    return pair.second + indexed[0] + consume(escaping);
}
