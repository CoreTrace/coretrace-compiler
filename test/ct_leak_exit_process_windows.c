// SPDX-License-Identifier: Apache-2.0
// A program that ends with ExitProcess, which runs neither its exit handlers nor its
// terminators: the leak report must run all the same and report its one block (#187).
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    char* volatile block = malloc(16);
    block[0] = 1;
    puts("ok");
    fflush(stdout);
    ExitProcess(0);
}
