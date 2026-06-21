#include "Arduino.h"

#include <stdarg.h>
#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/printk.h>

Nrf54Serial Serial;

uint32_t millis()
{
    return (uint32_t)k_uptime_get_32();
}

uint32_t micros()
{
    return (uint32_t)k_cyc_to_us_floor64(k_cycle_get_64());
}

void delay(uint32_t ms)
{
    k_sleep(K_MSEC(ms));
}

void delayMicroseconds(uint32_t us)
{
    k_busy_wait(us);
}

void yield()
{
    k_yield();
}

void pinMode(uint32_t pin, uint32_t mode)
{
    (void)pin;
    (void)mode;
}

void digitalWrite(uint32_t pin, uint32_t value)
{
    (void)pin;
    (void)value;
}

int digitalRead(uint32_t pin)
{
    (void)pin;
    return LOW;
}

long pulseIn(uint32_t pin, uint32_t state, uint32_t timeout)
{
    (void)pin;
    (void)state;
    (void)timeout;
    return 0;
}

void tone(uint32_t pin, unsigned int frequency, uint32_t duration)
{
    (void)pin;
    (void)frequency;
    (void)duration;
}

void noTone(uint32_t pin)
{
    (void)pin;
}

void attachInterrupt(uint32_t pin, void (*callback)(void), uint32_t mode)
{
    (void)pin;
    (void)callback;
    (void)mode;
}

void detachInterrupt(uint32_t pin)
{
    (void)pin;
}

uint32_t digitalPinToInterrupt(uint32_t pin)
{
    return pin;
}

long random(long max)
{
    if (max <= 0) {
        return 0;
    }
    return (long)(sys_rand32_get() % (uint32_t)max);
}

long random(long min, long max)
{
    if (max <= min) {
        return min;
    }
    return min + random(max - min);
}

int setenv(const char *name, const char *value, int overwrite)
{
    (void)name;
    (void)value;
    (void)overwrite;
    return 0;
}

void tzset() {}

size_t Print::write(uint8_t c)
{
    printk("%c", c);
    return 1;
}

size_t Print::write(const uint8_t *buffer, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        write(buffer[i]);
    }
    return size;
}

size_t Print::write(const char *str)
{
    if (str == nullptr) {
        return 0;
    }
    printk("%s", str);
    return strlen(str);
}

int Print::printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintk(format, args);
    va_end(args);
    return 0;
}

int Print::println(const char *str)
{
    int rc = print(str);
    printk("\n");
    return rc + 1;
}

int Print::print(const char *str)
{
    return (int)write(str);
}
