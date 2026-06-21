#include "SPI.h"

#include <string.h>

SPIClass SPI;
SPIClass SPI1(HSPI);

void SPIClass::transfer(const uint8_t *out, uint8_t *in, size_t len)
{
    if (in && out) {
        memcpy(in, out, len);
    } else if (in) {
        memset(in, 0, len);
    }
}
