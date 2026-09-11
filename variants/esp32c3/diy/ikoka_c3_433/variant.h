#ifndef _VARIANT_ESP32C3_SUPER_MINI_
#define _VARIANT_ESP32C3_SUPER_MINI_

/*----------------------------------------------------------------------------
 *        Headers
 *----------------------------------------------------------------------------*/

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

// I2C (Wire) & OLED
#define WIRE_INTERFACES_COUNT (1)
#define I2C_SDA (21)
#define I2C_SCL (20)





// Button
#define BUTTON_PIN (1) // BOOT button

// LoRa

#define USE_SX1268
// #define USE_RF95


#define LORA_DIO0 RADIOLIB_NC
#define LORA_RESET (4)
#define LORA_DIO1 (3)
// #define SX126X_POWER_EN (7)
#define LORA_BUSY (5)
#define LORA_SCK (8)
#define LORA_MISO (9)
#define LORA_MOSI (10)
#define LORA_RXEN (7)
#define LORA_CS (6)
#define LORA_DIO2 -1
#define SX126X_CS LORA_CS
#define SX126X_DIO1 LORA_DIO1
#define SX126X_BUSY LORA_BUSY
#define SX126X_RESET LORA_RESET
#define SX126X_RXEN LORA_RXEN
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_TXEN RADIOLIB_NC
#define SX126X_DIO3_TCXO_VOLTAGE (1.8)
#define SX126X_MAX_POWER 20
#define TCXO_OPTIONAL // make it so that the firmware can try both TCXO and XTAL

#ifdef __cplusplus
}
#endif




/*----------------------------------------------------------------------------
 *        Arduino objects - C++ only
 *----------------------------------------------------------------------------*/

#endif
