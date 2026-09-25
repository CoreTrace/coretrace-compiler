// SPDX-License-Identifier: Apache-2.0
// Objective-C objects allocated through objc_alloc, objc_alloc_init and objc_msgSend
// (new, and allocWithZone: in -copyWithZone:). The four objects kept in globals are
// still alive at exit, the only leaks reported; the others are released, with or
// without ARC.
#import <Foundation/Foundation.h>

@interface Node : NSObject <NSCopying>
@property(nonatomic) int value;
- (instancetype)initWithValue:(int)value;
@end

@implementation Node
- (instancetype)initWithValue:(int)value
{
    if ((self = [super init]))
        _value = value;
    return self;
}

- (id)copyWithZone:(NSZone*)zone
{
    return [[[self class] allocWithZone:zone] initWithValue:self.value];
}
@end

static Node* keptByAlloc;
static Node* keptByAllocInit;
static Node* keptByNew;
static Node* keptByCopy;

int main(void)
{
    @autoreleasepool
    {
        Node* released = [[Node alloc] init];
        Node* releasedCopy = [released copy];
        // A class cluster: +alloc returns a shared placeholder, not a new NSString.
        NSString* text = [[NSString alloc] initWithFormat:@"%d", released.value];
#if !__has_feature(objc_arc)
        [released release];
        [releasedCopy release];
        [text release];
#endif

        keptByAlloc = [[Node alloc] initWithValue:1]; // objc_alloc
        keptByAllocInit = [[Node alloc] init];        // objc_alloc_init
        keptByNew = [Node new];                       // objc_msgSend(new)
        keptByCopy = [keptByNew copy];                // objc_msgSend(allocWithZone:)
    }
    return 0;
}
