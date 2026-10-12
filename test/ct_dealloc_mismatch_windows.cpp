// SPDX-License-Identifier: Apache-2.0
// An array released with delete instead of delete[]. The Windows runtime records how each
// block was allocated: it reports the mismatch, with the kind it expected for that release,
// the kind it recorded and the site of the allocation, and still releases the block (#188).
int main()
{
    int* volatile values = new int[4];
    values[0] = 1;
    delete values;
    return 0;
}
