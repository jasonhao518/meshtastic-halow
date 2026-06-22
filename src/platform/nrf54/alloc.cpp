#include <cstddef>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static void *nrf54Alloc(std::size_t size, const char *kind)
{
    if (size >= 256) {
        printk("nrf54_alloc:%s:%u:begin\n", kind, (unsigned)size);
    }
    void *p = k_malloc(size);
    if (size >= 256 || !p) {
        printk("nrf54_alloc:%s:%u:%p\n", kind, (unsigned)size, p);
    }
    if (!p) {
        k_panic();
    }
    return p;
}

void *operator new(std::size_t size)
{
    return nrf54Alloc(size, "new");
}

void *operator new[](std::size_t size)
{
    return nrf54Alloc(size, "new[]");
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
