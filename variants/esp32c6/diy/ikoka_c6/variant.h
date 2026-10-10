/*
IKOKA STICK EBYTE E22-900M + XIAO-ESP32-C6
https://github.com/ndoo/ikoka-stick-meshtastic-device

XIAO pin -> ESP32-C6 GPIO
D0=0 D1=1 D2=2 D3=21 D4=22 D5=23 D6=16 D7=17 D8=19 D9=20 D10=18
*/

#pragma once

#define LED_POWER 15 // XIAO user LED
#define LED_STATE_ON 0

#define BUTTON_PIN 0 // D0, Ikoka Stick user button
#define BUTTON_NEED_PULLUP

// SSD1306 OLED
#define I2C_SDA 16 // D6
#define I2C_SCL 17 // D7

#define USE_SX1262

#define LORA_SCK 19  // D8
#define LORA_MISO 20 // D9
#define LORA_MOSI 18 // D10
#define LORA_CS 22   // D4
#define LORA_RESET 2 // D2
#define LORA_DIO1 1  // D1
#define LORA_BUSY 21 // D3
#define LORA_RXEN 23 // D5

#define SX126X_CS LORA_CS
#define SX126X_DIO1 LORA_DIO1
#define SX126X_BUSY LORA_BUSY
#define SX126X_RESET LORA_RESET
#define SX126X_RXEN LORA_RXEN
#define SX126X_TXEN RADIOLIB_NC
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_DIO3_TCXO_VOLTAGE 1.8
#define SX126X_MAX_POWER 20

// The C6 has only two UARTs, so the serial module cannot use Serial2.
#define SERIAL_PRINT_PORT 1
