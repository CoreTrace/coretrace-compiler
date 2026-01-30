#include <string>

int main()
{
    std::string s = "hello";
    return (static_cast<int>(s.size()) == 5) ? 0 : 1;
}
