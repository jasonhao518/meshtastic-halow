#pragma once

#include <stddef.h>
#include <stdint.h>

#define MSBFIRST 1
#define LSBFIRST 0
#define SPI_MODE0 0
#define HSPI 1

class SPISettings {
  public:
    SPISettings(uint32_t clock = 4000000, uint8_t bitOrder = MSBFIRST, uint8_t dataMode = SPI_MODE0)
        : clock(clock), bitOrder(bitOrder), dataMode(dataMode)
    {
    }

    uint32_t clock;
    uint8_t bitOrder;
    uint8_t dataMode;
};

class SPIClass {
  public:
    explicit SPIClass(int bus = 0) { (void)bus; }

    void begin() {}
    void begin(bool) {}
    void begin(int8_t, int8_t, int8_t, int8_t = -1) {}
    void end() {}
    void setSCK(int) {}
    void setTX(int) {}
    void setRX(int) {}
    void setPins(int, int, int) {}
    void setFrequency(uint32_t) {}
    void beginTransaction(SPISettings) {}
    void endTransaction() {}
    uint8_t transfer(uint8_t data) { return data; }
    void transfer(const uint8_t *out, uint8_t *in, size_t len);
};

extern SPIClass SPI;
extern SPIClass SPI1;
