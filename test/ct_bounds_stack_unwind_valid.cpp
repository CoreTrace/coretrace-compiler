// SPDX-License-Identifier: Apache-2.0
// An exception unwinds frames whose arrays are checked, skipping their exits; the
// frames called afterwards reuse that stack and must see only their own arrays.
#include <stdexcept>

static int deep(int depth)
{
    int buffer[8] = {};
    buffer[depth % 8] = depth;
    if (depth == 0)
        throw std::runtime_error("unwind");
    return deep(depth - 1) + buffer[depth % 8];
}

static int after(int base)
{
    int data[4];
    for (int i = 0; i < 4; ++i)
        data[i] = base + i;
    return data[3];
}

int main()
{
    try
    {
        deep(16);
    }
    catch (const std::exception&)
    {
    }
    return after(1) == 4 && after(10) == 13 ? 0 : 1;
}
