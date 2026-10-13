// SPDX-License-Identifier: Apache-2.0
//
// The index of blocks by address behind the allocation table's containing lookup (#185).
// Each test has an index of its own; nothing is dereferenced.
#include "ct_runtime_block_index.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

namespace
{
    // An index that starts small, so that the tests make it grow.
    struct Index
    {
        static constexpr size_t kInitialCapacity = 16;
        alignas(uintptr_t) unsigned char storage[ct_block_index_storage_size(kInitialCapacity)];
        ct_block_index index;

        Index()
        {
            ct_block_index_init(&index, storage, kInitialCapacity);
        }
        ~Index()
        {
            ct_block_index_destroy(&index);
        }

        bool find(uintptr_t address, uintptr_t* base = nullptr, size_t* size = nullptr) const
        {
            uintptr_t found_base = 0;
            size_t found_size = 0;
            const bool found = ct_block_index_find(&index, address, &found_base, &found_size);
            if (base)
            {
                *base = found_base;
            }
            if (size)
            {
                *size = found_size;
            }
            return found;
        }
    };

    TEST(BlockIndex, FindsTheBlockOfEveryByteAndNoneOutside)
    {
        Index index;
        // Sizes on both sides of the class boundaries, up to a large block.
        const size_t sizes[] = {1, 15, 16, 17, 255, 256, 257, 4096, 4097, 1u << 20};
        uintptr_t base = 0x10000000u;
        for (size_t size : sizes)
        {
            ASSERT_TRUE(ct_block_index_add(&index.index, base, size)) << size;
            uintptr_t found_base = 0;
            size_t found_size = 0;
            for (uintptr_t address : {base, base + size / 2, base + size - 1})
            {
                ASSERT_TRUE(index.find(address, &found_base, &found_size)) << size;
                EXPECT_EQ(found_base, base);
                EXPECT_EQ(found_size, size);
            }
            EXPECT_FALSE(index.find(base - 1)) << size;
            EXPECT_FALSE(index.find(base + size)) << size;
            // The next block starts two of its sizes further, aligned to 16 bytes.
            base += ((2 * size + 15) / 16) * 16 + 16;
        }
    }

    TEST(BlockIndex, RemovesOnlyThatBlock)
    {
        Index index;
        const uintptr_t first = 0x20000000u;
        const uintptr_t second = first + 32;
        ASSERT_TRUE(ct_block_index_add(&index.index, first, 32));
        ASSERT_TRUE(ct_block_index_add(&index.index, second, 32));

        ct_block_index_remove(&index.index, first, 32);
        EXPECT_FALSE(index.find(first + 8));
        uintptr_t base = 0;
        ASSERT_TRUE(index.find(second + 8, &base));
        EXPECT_EQ(base, second);

        // Removing a block the index does not hold changes nothing.
        ct_block_index_remove(&index.index, first, 32);
        ct_block_index_remove(&index.index, second, 64);
        EXPECT_TRUE(index.find(second + 8));
    }

    TEST(BlockIndex, AnAddressAddedAgainTakesItsNewSize)
    {
        Index index;
        const uintptr_t base = 0x30000000u;
        ASSERT_TRUE(ct_block_index_add(&index.index, base, 256));
        ct_block_index_remove(&index.index, base, 256);
        ASSERT_TRUE(ct_block_index_add(&index.index, base, 32));

        size_t size = 0;
        ASSERT_TRUE(index.find(base + 16, nullptr, &size));
        EXPECT_EQ(size, 32u);
        EXPECT_FALSE(index.find(base + 100));
    }

    TEST(BlockIndex, GrowsPastItsInitialCapacityAndKeepsEveryBlock)
    {
        Index index;
        constexpr uintptr_t kCount = 100000;
        for (uintptr_t i = 0; i < kCount; ++i)
        {
            ASSERT_TRUE(ct_block_index_add(&index.index, 0x40000000u + i * 48, 40)) << i;
        }
        EXPECT_FALSE(index.index.incomplete);
        for (uintptr_t i = 0; i < kCount; ++i)
        {
            uintptr_t base = 0;
            ASSERT_TRUE(index.find(0x40000000u + i * 48 + 39, &base)) << i;
            ASSERT_EQ(base, 0x40000000u + i * 48);
            ASSERT_FALSE(index.find(0x40000000u + i * 48 + 40)) << i;
        }
        for (uintptr_t i = 0; i < kCount; ++i)
        {
            ct_block_index_remove(&index.index, 0x40000000u + i * 48, 40);
        }
        EXPECT_FALSE(index.find(0x40000000u + 8));
        EXPECT_EQ(index.index.classes, 0u);
    }

    TEST(BlockIndex, ReusesTheSlotsOfRemovedBlocks)
    {
        Index index;
        for (int round = 0; round < 1000; ++round)
        {
            for (uintptr_t i = 0; i < Index::kInitialCapacity; ++i)
            {
                ASSERT_TRUE(ct_block_index_add(&index.index, 0x50000000u + i * 64, 64));
            }
            for (uintptr_t i = 0; i < Index::kInitialCapacity; ++i)
            {
                ct_block_index_remove(&index.index, 0x50000000u + i * 64, 64);
            }
        }
        EXPECT_EQ(index.index.capacity, Index::kInitialCapacity);
    }

    // Blocks of random sizes, apart from one another, and random addresses in and around them:
    // the index answers as a pass over every block does.
    TEST(BlockIndex, AnswersAsAPassOverEveryBlock)
    {
        struct Block
        {
            uintptr_t base;
            size_t size;
        };
        std::mt19937_64 random(185);
        std::vector<Block> blocks;
        Index index;
        uintptr_t next = 0x60000000u;
        for (int i = 0; i < 20000; ++i)
        {
            const uint64_t pick = random() % 100;
            const size_t size = pick < 90   ? 1 + random() % 512
                                : pick < 99 ? 4096 + random() % 8192
                                            : (1u << 20) + random() % 4096;
            blocks.push_back({next, size});
            ASSERT_TRUE(ct_block_index_add(&index.index, next, size));
            next += ((size + 15) / 16) * 16 + 16 * (random() % 4);
        }
        // Remove a third of them.
        for (size_t i = 0; i < blocks.size(); i += 3)
        {
            ct_block_index_remove(&index.index, blocks[i].base, blocks[i].size);
            blocks[i].size = 0;
        }

        for (int probe = 0; probe < 50000; ++probe)
        {
            const uintptr_t address = 0x60000000u - 64 + random() % (next - 0x60000000u + 128);
            const Block* expected = nullptr;
            for (const Block& block : blocks)
            {
                if (block.size != 0 && address - block.base < block.size)
                {
                    expected = &block;
                    break;
                }
            }
            uintptr_t base = 0;
            size_t size = 0;
            const bool found = index.find(address, &base, &size);
            ASSERT_EQ(found, expected != nullptr) << std::hex << address;
            if (expected)
            {
                ASSERT_EQ(base, expected->base) << std::hex << address;
                ASSERT_EQ(size, expected->size) << std::hex << address;
            }
        }
    }
} // namespace
