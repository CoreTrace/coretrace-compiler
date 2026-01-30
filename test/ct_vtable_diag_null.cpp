#include <cstdio>

extern "C" void __ct_vtable_dump(void* this_ptr, const char* site, const char* static_type);

int main()
{
    std::puts("ct_vtable_diag_null");
    __ct_vtable_dump(nullptr, "ct_vtable_diag_null.cpp:7:5", "Base");
    return 0;
}
