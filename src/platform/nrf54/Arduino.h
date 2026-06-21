#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string>
#include <zephyr/kernel.h>

using String = std::string;

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

uint32_t millis();
void delay(uint32_t ms);
void pinMode(uint32_t pin, uint32_t mode);
void digitalWrite(uint32_t pin, uint32_t value);
int digitalRead(uint32_t pin);
long random(long max);
long random(long min, long max);

class Print {
  public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t c);
    virtual size_t write(const uint8_t *buffer, size_t size);
    size_t write(const char *str);
    int printf(const char *format, ...);
    int println(const char *str = "");
    int print(const char *str);
};

class Stream : public Print {};

class Nrf54Serial : public Stream {
  public:
    void begin(unsigned long baud) { (void)baud; }
    operator bool() const { return true; }
};

extern Nrf54Serial Serial;
