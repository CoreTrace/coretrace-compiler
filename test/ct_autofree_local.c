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

// `malloc(sizeof(void*));` -> pas stocké -> unreachable -> autofree instant
// `void* ptr_leaked = malloc(sizeof(void*));` -> stocké -> reachable dans le scope de la fonction (à préciser où il est reachable)
//      Si il est stocké dans une variable global -> mises à jour du status reachable func à reachable global.
//      Si il reste dans la fonction -> pas de free -> check à la fin de la fonction le status reachable global/local -> si local autofree à la fin de la fonction
//      -> si global = ne rien faire

// CT_AUTOFREE_SCAN_PTR=1 \
// CT_AUTOFREE_SCAN_GLOBALS=0 \
// CT_AUTOFREE_SCAN_STACK=1 \
// CT_AUTOFREE_SCAN_REGS=0 \
// CT_AUTOFREE_SCAN_PERIOD_NS=10 \
// CT_AUTOFREE_SCAN_BUDGET_MS=1000 \
// CT_DEBUG_AUTOFREE_SCAN=1 CT_AUTOFREE_SCAN_INTERIOR=0 \
// /tmp/ct_test