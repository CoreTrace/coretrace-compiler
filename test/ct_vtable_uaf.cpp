// test/ct_vtable_uaf.cpp
#include <cstdio>
struct Base
{
    virtual ~Base() = default;
    virtual int value() const
    {
        return 1;
    }
};
struct Derived : Base
{
    int value() const override
    {
        return 2;
    }
};

int main()
{
    Base* ptr = new Derived();
    delete ptr;
    // UAF volontaire pour tester le diag
    std::printf("%d\n", ptr->value());
    return 0;
}
