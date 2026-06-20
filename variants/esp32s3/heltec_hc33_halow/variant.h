/*
 * Heltec HT-HC33 Wi-Fi HaLow device.
 *
 * This variant deliberately ships without LoRa, GPS, or the SSD1306 OLED. The
 * HaLow module replaces the RF path and owns its SPI/control pins directly.
 */

#define LED_POWER 48
#define LED_STATE_ON 1

#define BUTTON_PIN 21
#define BUTTON_NEED_PULLUP

// No battery monitoring on this variant — leaving BATTERY_PIN undefined skips
// the legacy adc1_* ADC code in Power.cpp that isn't compatible with IDF 5.1.

// Disable I2C, the OLED screen, and GPS so nothing else drives HaLow pins.
#define HAS_WIRE 0
#define HAS_SCREEN 0
#define HAS_GPS 0
#define NO_GPS 1

// Heltec HT-HC33 module pin map.
#define HALOW_SPI_SCK 4
#define HALOW_SPI_MISO 5
#define HALOW_SPI_MOSI 3
#define HALOW_CS 2
#define HALOW_IRQ 6
#define HALOW_RST 8
#define HALOW_WAKE 9
#define HALOW_BUSY 7
