// SPDX-License-Identifier: Apache-2.0
// No headers: the global allocation functions are implicitly declared, so this file
// compiles for any target, including a Windows target from another host.
int main()
{
    int* freed = new int(1);
    delete freed;
    int* array = new int[4];
    delete[] array;
    int* leaked = new int(7); // never deleted: the only leak reported at exit
    return *leaked == 7 ? 0 : 2;
}
