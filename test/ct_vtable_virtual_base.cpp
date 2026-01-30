#include <cstdio>

struct VBase
{
    virtual ~VBase() = default;
    virtual int value() const
    {
        return 1;
    }
};

struct Left : virtual VBase
{
    int value() const override
    {
        return 11;
    }
};

struct Right : virtual VBase
{
    int value() const override
    {
        return 12;
    }
};

struct Most : Left, Right
{
    int value() const override
    {
        return 99;
    }
};

int main()
{
    Most obj;
    VBase* base = &obj;
    std::printf("value=%d\n", base->value());
    return 0;
}
