#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>

// Seeed's XIAO_ESP32C6 defaults put I2C on D4/D5, which the Ikoka Stick uses for LoRa NSS/RXEN.
// I2C is on D6/D7 here, so the Arduino defaults follow the stick's wiring instead.

static const uint8_t SDA = 16; // D6
static const uint8_t SCL = 17; // D7

// Default SPI will be mapped to Radio
static const uint8_t SS = 22;   // D4
static const uint8_t MOSI = 18; // D10
static const uint8_t MISO = 20; // D9
static const uint8_t SCK = 19;  // D8

static const uint8_t A0 = 0;
static const uint8_t A1 = 1;
static const uint8_t A2 = 2;

// Wi-Fi RF switch, driven by the core's XIAO_ESP32C6 initVariant(): power on, on-board antenna.
static const uint8_t WIFI_ENABLE = 3;
static const uint8_t WIFI_ANT_CONFIG = 14;

#endif /* Pins_Arduino_h */
