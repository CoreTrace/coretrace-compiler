#include <cstdio>

struct Base1 {
    virtual ~Base1() = default;
    virtual const char *name1() const { return "Base1"; }
};

struct Base2 {
    virtual ~Base2() = default;
    virtual int id2() const { return 2; }
};

struct Derived : Base1, Base2 {
    const char *name1() const override { return "Derived"; }
    int id2() const override { return 42; }
};

int main()
{
    Derived obj;
    Base1 *b1 = &obj;
    Base2 *b2 = &obj;
    std::printf("%s %d\n", b1->name1(), b2->id2());
    return 0;
}
