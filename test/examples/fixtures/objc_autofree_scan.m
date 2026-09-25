// SPDX-License-Identifier: Apache-2.0
// An Objective-C object nothing refers to any more while the conservative scan runs
// every millisecond. The scan must leave it to the Objective-C runtime: it is still
// reported as a leak at exit, not freed behind the runtime's back.
#import <Foundation/Foundation.h>
#include <string.h>
#include <unistd.h>

@interface Node : NSObject
@end

@implementation Node
@end

__attribute__((noinline)) static void lose(void)
{
    (void)[Node new]; // never released, and no reference to it outlives this call
}

// Overwrites the stack the call above used, where a copy of the pointer may remain.
__attribute__((noinline)) static void clobberStack(void)
{
    volatile char stack[8192];
    memset((char*)stack, 0, sizeof stack);
}

int main(void)
{
    lose();
    clobberStack();
    usleep(300000); // the periodic scan runs meanwhile
    return 0;
}
