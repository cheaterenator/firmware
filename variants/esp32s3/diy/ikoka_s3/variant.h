/*
IKOKA STICK EBYTE + XIAO-S3
*/



#define LED_PIN 48
#define LED_STATE_ON 1 // State when LED is lit

#define BUTTON_PIN 1 // D0, Ikoka Stick user button (100k pull-up on the stick)
#define BUTTON_NEED_PULLUP

// No battery sense on the stick. Defining BATTERY_PIN (even as -1) makes Power.cpp claim ADC_CHANNEL,
// and ADC1 channel 0 is GPIO1 = D0, the user button.

#define I2C_SDA 43
#define I2C_SCL 44

// XIAO S3 LORA module
#define USE_SX1262

#define LORA_MISO 8
#define LORA_SCK 7
#define LORA_MOSI 9
#define LORA_CS 5

#define LORA_RESET 3
#define LORA_DIO1 2

#define LORA_DIO2 -1

#ifdef USE_SX1262
#define SX126X_CS LORA_CS
#define SX126X_DIO1 LORA_DIO1
#define SX126X_BUSY 4
#define SX126X_RESET LORA_RESET


//  DIO2 controlls an antenna switch and the TCXO voltage is controlled by DIO3
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_RXEN 6
#define SX126X_TXEN RADIOLIB_NC
#define SX126X_DIO3_TCXO_VOLTAGE 1.8
#define SX126X_MAX_POWER 20
#endif
