#include <cstdio>

struct Base {
    virtual ~Base() = default;
    virtual int value() const { return 1; }
};

struct Derived : Base {
    int value() const override { return 2; }
};

int main()
{
    Base *ptr = new Derived();
    int out = ptr->value();
    std::printf("value=%d\n", out);
    delete ptr;
    return 0;
}
