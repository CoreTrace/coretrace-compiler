#include <cstdio>

extern "C" void __ct_vcall_trace(void* this_ptr, void* target, const char* site,
                                 const char* static_type);

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
    std::puts("ct_vtable_diag_stack_target");

    Derived obj;
    Base* base = &obj;
    int local = 42;

    __ct_vcall_trace(base, &local, "ct_vtable_diag_stack_target.cpp:25:5", "Derived");

    return 0;
}
