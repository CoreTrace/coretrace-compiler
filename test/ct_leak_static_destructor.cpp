// SPDX-License-Identifier: Apache-2.0
// Blocks the program releases in its own exit-time code: in the destructors of two
// global objects, one of which allocated its block in main, and in an exit handler
// registered in main. The leak report must come after them and find nothing.
#include <cstdio>
#include <cstdlib>

namespace
{
    struct Owner
    {
        char* block = nullptr;
        ~Owner()
        {
            std::free(block);
        }
    };

    struct Allocator
    {
        char* block = static_cast<char*>(std::malloc(16));
        ~Allocator()
        {
            std::free(block);
        }
    };

    Allocator allocated_at_startup;
    Owner allocated_in_main;
    char* released_by_handler = nullptr;

    void release_at_exit()
    {
        std::free(released_by_handler);
    }
} // namespace

int main()
{
    allocated_in_main.block = static_cast<char*>(std::malloc(32));
    released_by_handler = static_cast<char*>(std::malloc(48));
    if (std::atexit(release_at_exit) != 0)
    {
        return 2;
    }
    allocated_at_startup.block[0] = 1;
    std::puts("ok");
    return 0;
}
