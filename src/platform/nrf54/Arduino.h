#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <strings.h>
#include <zephyr/kernel.h>

using String = std::string;
using uint = unsigned int;
using std::max;
using std::min;
using std::lround;
using std::round;
using byte = uint8_t;

class __FlashStringHelper;

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define RISING 1
#define FALLING 2
#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2
#define PROGMEM
#define F(str) (reinterpret_cast<const __FlashStringHelper *>(str))

using PGM_P = const char *;

static inline uint8_t pgm_read_byte(const void *addr)
{
    return *reinterpret_cast<const uint8_t *>(addr);
}

static inline uint32_t pgm_read_dword(const void *addr)
{
    return *reinterpret_cast<const uint32_t *>(addr);
}

uint32_t millis();
uint32_t micros();
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
void yield();
void pinMode(uint32_t pin, uint32_t mode);
void digitalWrite(uint32_t pin, uint32_t value);
int digitalRead(uint32_t pin);
long pulseIn(uint32_t pin, uint32_t state, uint32_t timeout = 1000000);
void tone(uint32_t pin, unsigned int frequency, uint32_t duration = 0);
void noTone(uint32_t pin);
void attachInterrupt(uint32_t pin, void (*callback)(void), uint32_t mode);
void detachInterrupt(uint32_t pin);
uint32_t digitalPinToInterrupt(uint32_t pin);
long random(long max);
long random(long min, long max);
long random();
long map(long value, long fromLow, long fromHigh, long toLow, long toHigh);
int setenv(const char *name, const char *value, int overwrite);
void tzset();

class Print {
  public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t c);
    virtual size_t write(const uint8_t *buffer, size_t size);
    size_t write(const char *buffer, size_t size);
    size_t write(const char *str);
    int printf(const char *format, ...);
    int println(const char *str = "");
    int println(const String &str);
    int print(const char *str);
    int print(const String &str);
};

class Stream : public Print {
  public:
    virtual int available() { return 0; }
    virtual int read() { return -1; }
    virtual void flush() {}
};

class Nrf54Serial : public Stream {
  public:
    void begin(unsigned long baud) { (void)baud; }
    operator bool() const { return true; }
};

extern Nrf54Serial Serial;
