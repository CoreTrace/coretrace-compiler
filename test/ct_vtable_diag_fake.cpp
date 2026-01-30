#include <cstdio>
#include <cstdlib>

extern "C" void __ct_vtable_dump(void* this_ptr, const char* site, const char* static_type);

struct FakeObject
{
    void** vptr;
};

int main()
{
    std::puts("ct_vtable_diag_fake");

    void** table = static_cast<void**>(std::calloc(4, sizeof(void*)));
    if (!table)
    {
        return 1;
    }

    table[0] = nullptr; // offset-to-top
    table[1] = nullptr; // missing typeinfo
    table[2] = nullptr; // fake entry

    FakeObject obj{};
    obj.vptr = &table[2]; // vtable pointer points to heap (unresolvable module)

    __ct_vtable_dump(&obj, "ct_vtable_diag_fake.cpp:22:5", "FakeObject");

    std::free(table);
    return 0;
}
