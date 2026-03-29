// SPDX-License-Identifier: Apache-2.0
struct Base
{
    virtual ~Base() = default;
    virtual int foo()
    {
        return 1;
    }
};

int main()
{
    Base b;
    return b.foo();
}
