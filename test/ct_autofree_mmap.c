#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void* p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
    {
        return 0;
    }
    (void)p;
    return 0;
}
