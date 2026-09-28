// SPDX-License-Identifier: Apache-2.0
// Standard containers and strings, smart pointers, virtual dispatch through owning
// pointers, and an exception that unwinds a frame holding a local array.
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    struct Shape
    {
        virtual ~Shape() = default;
        virtual double area() const = 0;
        virtual char tag() const = 0;
    };

    struct Rectangle final : Shape
    {
        Rectangle(double width, double height) : width(width), height(height) {}
        double area() const override
        {
            return width * height;
        }
        char tag() const override
        {
            return 'r';
        }
        double width;
        double height;
    };

    struct Circle final : Shape
    {
        explicit Circle(double radius) : radius(radius) {}
        double area() const override
        {
            return 3.14159265358979 * radius * radius;
        }
        char tag() const override
        {
            return 'c';
        }
        double radius;
    };

    int windowed_sum(const std::vector<int>& values, std::size_t limit)
    {
        int window[8] = {};
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i >= limit)
            {
                throw std::out_of_range("past limit " + std::to_string(limit));
            }
            window[i % 8] += values[i];
        }
        return std::accumulate(std::begin(window), std::end(window), 0);
    }
} // namespace

int main()
{
    std::vector<int> values;
    for (int i = 0; i < 1000; ++i)
    {
        values.push_back((i * 37) % 101);
    }
    std::sort(values.begin(), values.end(), [](int a, int b) { return a > b; });

    std::map<std::string, int> buckets;
    for (int value : values)
    {
        ++buckets["bucket-" + std::to_string(value % 7)];
    }

    std::vector<std::unique_ptr<Shape>> shapes;
    for (int i = 1; i <= 5; ++i)
    {
        shapes.push_back(std::make_unique<Rectangle>(i, i + 1));
        shapes.push_back(std::make_unique<Circle>(i));
    }
    double total_area = 0.0;
    std::string tags;
    for (const auto& shape : shapes)
    {
        total_area += shape->area();
        tags += shape->tag();
    }

    auto shared = std::make_shared<std::vector<int>>(values.begin(), values.begin() + 10);
    std::shared_ptr<std::vector<int>> kept = shared;
    shared.reset();

    std::string message;
    try
    {
        windowed_sum(values, 500);
    }
    catch (const std::out_of_range& error)
    {
        message = error.what();
    }
    int sum = windowed_sum(values, values.size());

    std::printf("first=%d last=%d buckets=%zu bucket-3=%d area=%.3f tags=%s kept=%zu/%ld "
                "error='%s' sum=%d\n",
                values.front(), values.back(), buckets.size(), buckets["bucket-3"], total_area,
                tags.c_str(), kept->size(), static_cast<long>(kept.use_count()), message.c_str(),
                sum);
    return 0;
}
