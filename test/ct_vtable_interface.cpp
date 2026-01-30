#include <cstdio>

struct IFace
{
    virtual ~IFace() = default;
    virtual int run(int value) = 0;
};

struct Impl : IFace
{
    int run(int value) override
    {
        return value * 3;
    }
};

int main()
{
    IFace* ptr = new Impl();
    int out = ptr->run(7);
    std::printf("run=%d\n", out);
    delete ptr;
    return 0;
}
