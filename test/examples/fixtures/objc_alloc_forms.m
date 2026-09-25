// SPDX-License-Identifier: Apache-2.0
// No headers: a root class declares the methods, so this file compiles to IR for any
// target from any host. One allocation per form clang emits, and messages that are not
// allocations.
__attribute__((objc_root_class))
@interface Node
+ (instancetype)alloc;
+ (instancetype)allocWithZone:(void*)zone;
+ (instancetype)new;
- (instancetype)init;
- (Class)class;
@end

void makeNodes(void* zone)
{
    Node* byAlloc = [Node alloc];                      // objc_alloc
    Node* byAllocWithZone = [Node allocWithZone:0];    // objc_allocWithZone
    Node* byAllocInit = [[Node alloc] init];           // objc_alloc_init
    Node* byNew = [Node new];                          // objc_msgSend(new)
    Node* byZone = [[byNew class] allocWithZone:zone]; // objc_msgSend(allocWithZone:)
    (void)byAlloc;
    (void)byAllocWithZone;
    (void)byAllocInit;
    (void)byZone;
}
