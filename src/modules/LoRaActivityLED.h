#pragma once

#include "configuration.h"

// Single WS2812/NeoPixel showing radio activity, enabled at build time by defining its data pin:
//   NEOPIXEL_LORA_PIN                  (required) GPIO driving the pixel
//   NEOPIXEL_LORA_BRIGHTNESS           0-255, default 32 (fixed, not runtime-configurable)
//   NEOPIXEL_LORA_TYPE                 default NEO_GRB + NEO_KHZ800
//   NEOPIXEL_LORA_TX_COLOR             default red   - lit for the whole transmission
//   NEOPIXEL_LORA_RX_COLOR             default green - flashed on every valid LoRa packet received
//   NEOPIXEL_LORA_HEARTBEAT_COLOR      default blue  - short blink while idle (off with device.led_heartbeat_disabled)
//   NEOPIXEL_LORA_RX_FLASH_MS          default 100
//   NEOPIXEL_LORA_TX_MIN_MS            default 50, so a short ACK on a fast preset is still visible
//   NEOPIXEL_LORA_HEARTBEAT_PERIOD_MS  default 1000
//   NEOPIXEL_LORA_HEARTBEAT_ON_MS      default 50
// Priority when several apply at once: TX > RX > heartbeat.
#ifdef NEOPIXEL_LORA_PIN

#include "concurrency/OSThread.h"
#include <Adafruit_NeoPixel.h>

#ifndef NEOPIXEL_LORA_BRIGHTNESS
#define NEOPIXEL_LORA_BRIGHTNESS 32
#endif
#ifndef NEOPIXEL_LORA_TYPE
#define NEOPIXEL_LORA_TYPE (NEO_GRB + NEO_KHZ800)
#endif
#ifndef NEOPIXEL_LORA_TX_COLOR
#define NEOPIXEL_LORA_TX_COLOR 0xFF0000 // red
#endif
#ifndef NEOPIXEL_LORA_RX_COLOR
#define NEOPIXEL_LORA_RX_COLOR 0x00FF00 // green
#endif
#ifndef NEOPIXEL_LORA_HEARTBEAT_COLOR
#define NEOPIXEL_LORA_HEARTBEAT_COLOR 0x0000FF // blue
#endif
#ifndef NEOPIXEL_LORA_RX_FLASH_MS
#define NEOPIXEL_LORA_RX_FLASH_MS 100
#endif
#ifndef NEOPIXEL_LORA_TX_MIN_MS
#define NEOPIXEL_LORA_TX_MIN_MS 50
#endif
#ifndef NEOPIXEL_LORA_HEARTBEAT_PERIOD_MS
#define NEOPIXEL_LORA_HEARTBEAT_PERIOD_MS 1000
#endif
#ifndef NEOPIXEL_LORA_HEARTBEAT_ON_MS
#define NEOPIXEL_LORA_HEARTBEAT_ON_MS 50
#endif

class LoRaActivityLED : private concurrency::OSThread
{
  public:
    LoRaActivityLED();

  protected:
    int32_t runOnce() override;

  private:
    Adafruit_NeoPixel pixel = Adafruit_NeoPixel(1, NEOPIXEL_LORA_PIN, NEOPIXEL_LORA_TYPE);
    uint32_t shownColor = 0;

    bool txActive = false;
    bool txHeld = false; // still showing TX after it ended, until NEOPIXEL_LORA_TX_MIN_MS have passed
    uint32_t txStart = 0;
    bool rxActive = false;
    uint32_t rxStart = 0;

    bool sleeping = false;

    int handleLoRaRx(uint32_t sender);
    int handleLoRaTx(bool started);
    int handleDeepSleep(void *unused);

    /// Pick the colour for the current state and push it to the pixel if it changed.
    /// Returns ms until the next state change we have to wake for.
    uint32_t refresh();

    CallbackObserver<LoRaActivityLED, uint32_t> loraRxObserver =
        CallbackObserver<LoRaActivityLED, uint32_t>(this, &LoRaActivityLED::handleLoRaRx);
    CallbackObserver<LoRaActivityLED, bool> loraTxObserver =
        CallbackObserver<LoRaActivityLED, bool>(this, &LoRaActivityLED::handleLoRaTx);
    CallbackObserver<LoRaActivityLED, void *> deepSleepObserver =
        CallbackObserver<LoRaActivityLED, void *>(this, &LoRaActivityLED::handleDeepSleep);
};

extern LoRaActivityLED *loraActivityLED;

#endif
