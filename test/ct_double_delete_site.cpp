// SPDX-License-Identifier: Apache-2.0
// Deletes one array twice: the runtime skips the second delete[] and reports where it
// happens and where the memory came from.
int main()
{
    int* volatile values = new int[4];
    delete[] values;
    delete[] values;
    return 0;
}
