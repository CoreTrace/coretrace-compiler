// SPDX-License-Identifier: Apache-2.0
#import <Foundation/Foundation.h>
#include <stdlib.h>

@interface Buffers : NSObject
- (char*)keep;
@end

@implementation Buffers
- (char*)keep
{
    char* freed = malloc(16);
    free(freed);
    return malloc(32); // never freed: the only leak reported at exit
}
@end

int main(void)
{
    char* leaked = [[[Buffers alloc] init] keep];
    if (!leaked)
        return 2;
    leaked[0] = 'x';
    return 0;
}
