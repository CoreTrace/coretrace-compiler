// SPDX-License-Identifier: Apache-2.0
#import <Foundation/Foundation.h>

@interface Counters : NSObject
- (int*)keep;
@end

@implementation Counters
- (int*)keep
{
    int* freed = new int(1);
    delete freed;
    int* array = new int[4];
    delete[] array;
    return new int(7); // never deleted: the only leak reported at exit
}
@end

int main()
{
    int* leaked = [[[Counters alloc] init] keep];
    return *leaked == 7 ? 0 : 2;
}
