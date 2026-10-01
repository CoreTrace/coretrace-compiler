// SPDX-License-Identifier: Apache-2.0
// A block the destructor of a global object releases at exit: the leak report must come
// after that destructor and find nothing. No headers, as in new_delete.cpp.
struct Owner
{
    int* block = new int(7);
    ~Owner()
    {
        delete block;
    }
};

Owner owner;

int main()
{
    return *owner.block == 7 ? 0 : 2;
}
