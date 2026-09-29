// SPDX-License-Identifier: Apache-2.0
// Blocks that user code allocates and the C++ library releases (std::unique_ptr), and
// library containers that allocate and release their own storage. Everything is released
// before exit, so no leak may be reported at any optimization level.
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

int main()
{
    std::unique_ptr<int> owned(new int(7));
    std::unique_ptr<int[]> array(new int[4]{1, 2, 3, 4});
    std::vector<std::unique_ptr<int>> pointers;
    for (int i = 0; i < 16; ++i)
    {
        pointers.push_back(std::unique_ptr<int>(new int(i)));
    }
    std::map<std::string, int> counts;
    for (int i = 0; i < 32; ++i)
    {
        ++counts["key-" + std::to_string(i % 5)];
    }
    auto shared = std::make_shared<std::vector<int>>(64, 1);

    std::printf("owned=%d array=%d pointers=%zu counts=%zu shared=%zu\n", *owned, array[3],
                pointers.size(), counts.size(), shared->size());
    return 0;
}
