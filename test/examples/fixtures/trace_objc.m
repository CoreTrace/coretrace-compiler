// SPDX-License-Identifier: Apache-2.0
#import <Foundation/Foundation.h>

@interface Greeter : NSObject
- (int)greet:(int)times;
@end

@implementation Greeter
- (int)greet:(int)times
{
    return times + 1;
}
@end

int main(void)
{
    Greeter* greeter = [[Greeter alloc] init];
    int result = [greeter greet:1];
    [greeter release];
    return result == 2 ? 0 : 1;
}
