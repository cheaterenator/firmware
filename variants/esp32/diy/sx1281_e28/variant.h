// ---------------------------------------------------------------------------
// DIY node: generic ESP32-WROOM-32 DevKit + EBYTE E28-2G4M12S/20S/27S
// (Semtech SX1281, 2.4 GHz LoRa/FLRC/GFSK) module.
//
// See README.md in this folder for the full wiring table, bill of materials
// and notes on antennas / RF power / regulatory limits.
//
// Note on SX1281 vs SX1280: Meshtastic's radio driver only ships a
// SX1280Interface (SX128xInterface<SX1280>), there is no separate SX1281
// class. The SX1281 is a register-compatible sibling of the SX1280 that
// simply omits the ranging engine - a feature Meshtastic doesn't use - so
// USE_SX1280 is the correct (and only) define for this module too.
// ---------------------------------------------------------------------------

// Reuse the DevKit's own "BOOT" button (already has a pull-up on the board,
// no extra wiring needed)
#define BUTTON_PIN 0

// Reuse the DevKit's onboard LED
#define LED_POWER 2

// Optional 0.96" I2C OLED (SSD1306). Comment out HAS_SCREEN/USE_SSD1306 if
// you aren't wiring one up.
#define HAS_SCREEN 1
#define USE_SSD1306
#define I2C_SDA 21
#define I2C_SCL 22

// Optional battery voltage sense via a resistor divider to an ADC-capable
// pin. Comment out BATTERY_PIN if you're running the node from USB only.
#define BATTERY_PIN 35
#define ADC_CHANNEL ADC_CHANNEL_7
#define ADC_MULTIPLIER 2.0 // set this to match whatever divider you actually build

// Optional GPS on UART2 (e.g. NEO-6M / NEO-M8N). Comment out the three
// GPS_*/PIN_GPS_EN lines below if you don't have one.
#define GPS_RX_PIN 16 // ESP32 RX  <- GPS module TX
#define GPS_TX_PIN 17 // ESP32 TX  -> GPS module RX
#define PIN_GPS_EN 4  // optional GPS enable/power-switch pin (or wire GPS VCC straight to 3V3 and drop this)
#define GPS_UBLOX     // remove/replace if using a non-u-blox GPS module

// ---------------------------------------------------------------------------
// Radio: EBYTE E28-2G4M12S / E28-2G4M20S / E28-2G4M27S (Semtech SX1281)
// ---------------------------------------------------------------------------
#define USE_SX1280

#define LORA_SCK 18
#define LORA_MISO 19
#define LORA_MOSI 23
#define LORA_CS 5
#define LORA_RESET 14

#define SX128X_CS LORA_CS
#define SX128X_RESET LORA_RESET
#define SX128X_BUSY 32
#define SX128X_DIO1 33
#define SX128X_RXEN 26 // -> module's RX_EN pin
#define SX128X_TXEN 27 // -> module's TX_EN pin

// Conservative default = the bare SX1281 chip's native max output (~12.5 dBm).
// The E28 module's internal PA takes it from there up to its rated 12/20/27 dBm.
// See README.md before raising this - most 2.4 GHz ISM regulations cap you
// around 20 dBm EIRP regardless of what the module itself supports.
#define SX128X_MAX_POWER 12
