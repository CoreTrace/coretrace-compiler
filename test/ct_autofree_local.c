#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

void* global;
void* global2;

static void foo(void)
{

    void* ptr_normally_freed = malloc(sizeof(void*) * 2);
    void* ptr_leaked = malloc(sizeof(void*) * 3);
    void* test = malloc(sizeof(void*) * 4);
    (void)ptr_leaked;
    malloc(sizeof(void*));
    global = ptr_leaked;
    global2 = ptr_normally_freed;
    printf("%p\n", test);
    // sleep(5);
}

void c()
{
    foo();
}

void b()
{
    c();
    malloc(sizeof(void*));
    char* p = malloc(100);
    char* q = p + 16; // pointeur “interior”
}

void a()
{
    b();
    // sleep(5);
}

int main(void)
{
    a();
    // printf("%p\n", global);
    // printf("%p\n", global2);
    // free(global);
    // free(global2);
    return 0;
}
