#include <cstdio>

extern "C" void __ct_vtable_dump(void *this_ptr, const char *site, const char *static_type);

struct Base {
    virtual ~Base() = default;
    virtual int value() const { return 1; }
};

struct Derived : Base {
    int value() const override { return 2; }
};

int main()
{
    std::puts("ct_vtable_diag_freed");

    Derived *ptr = new Derived();
    delete ptr;

    // UB: intentionally using a freed pointer to trigger the diagnostic.
    __ct_vtable_dump(ptr, "ct_vtable_diag_freed.cpp:22:5", "Derived");

    return 0;
}
