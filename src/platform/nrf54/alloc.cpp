#include <cstddef>
#include <zephyr/kernel.h>

void *operator new(std::size_t size)
{
    void *p = k_malloc(size);
    if (!p) {
        k_panic();
    }
    return p;
}

void *operator new[](std::size_t size)
{
    void *p = k_malloc(size);
    if (!p) {
        k_panic();
    }
    return p;
}

void operator delete(void *ptr) noexcept
{
    k_free(ptr);
}

void operator delete[](void *ptr) noexcept
{
    k_free(ptr);
}

void operator delete(void *ptr, std::size_t) noexcept
{
    k_free(ptr);
}

void operator delete[](void *ptr, std::size_t) noexcept
{
    k_free(ptr);
}

extern "C" void *__cxa_allocate_exception(std::size_t)
{
    k_panic();
    return nullptr;
}

extern "C" void __cxa_free_exception(void *)
{
}
