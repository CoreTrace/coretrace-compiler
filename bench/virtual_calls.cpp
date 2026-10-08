// SPDX-License-Identifier: Apache-2.0
// Overhead benchmark: virtual calls through base pointers to objects of three types.
// Prints a checksum, the same in every build.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace
{
    struct Shape
    {
        virtual ~Shape() = default;
        virtual long step(long value) const = 0;
    };

    struct Square : Shape
    {
        long step(long value) const override
        {
            return value * value % 1009;
        }
    };

    struct Shift : Shape
    {
        long step(long value) const override
        {
            return (value + 17) % 1013;
        }
    };

    struct Mix : Shape
    {
        long step(long value) const override
        {
            return (value * 31 + 7) % 1019;
        }
    };
} // namespace

int main(int argc, char** argv)
{
    const long rounds = argc > 1 ? std::atol(argv[1]) : 2000;
    std::vector<std::unique_ptr<Shape>> shapes;
    for (int i = 0; i < 1024; ++i)
    {
        if (i % 3 == 0)
            shapes.push_back(std::make_unique<Square>());
        else if (i % 3 == 1)
            shapes.push_back(std::make_unique<Shift>());
        else
            shapes.push_back(std::make_unique<Mix>());
    }
    long value = 1;
    for (long round = 0; round < rounds; ++round)
        for (const auto& shape : shapes)
            value = shape->step(value);
    std::printf("checksum=%ld\n", value);
    return 0;
}
