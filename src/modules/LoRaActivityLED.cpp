#include "LoRaActivityLED.h"

#ifdef NEOPIXEL_LORA_PIN

#include "configuration.h"
#include "main.h"
#include "mesh/RadioInterface.h"
#include "mesh/Throttle.h"
#include "sleep.h"

/*
LoRaActivityLED drives one NeoPixel as a radio activity indicator: TX for as long as the radio transmits, a short
flash per received packet, and a heartbeat blink while idle. RX/TX notifications arrive on the radio thread, which
shares the main loop with this thread, so the pixel can be written straight from the callbacks.
*/
LoRaActivityLED *loraActivityLED;

LoRaActivityLED::LoRaActivityLED() : concurrency::OSThread("LoRaActivityLED")
{
    pixel.begin();
    pixel.setBrightness(NEOPIXEL_LORA_BRIGHTNESS);
    pixel.clear();
    pixel.show();

    loraRxObserver.observe(&RadioInterface::loraRxPacketObservable);
    loraTxObserver.observe(&RadioInterface::loraTxObservable);
    deepSleepObserver.observe(&notifyDeepSleep);
    LOG_INFO("LoRa activity LED on GPIO %d, brightness %d", NEOPIXEL_LORA_PIN, NEOPIXEL_LORA_BRIGHTNESS);
}

int LoRaActivityLED::handleLoRaRx(uint32_t)
{
    rxActive = true;
    rxStart = millis();
    setIntervalFromNow(refresh());
    return 0;
}

int LoRaActivityLED::handleLoRaTx(bool started)
{
    if (started) {
        txActive = true;
        txHeld = true;
        txStart = millis();
    } else {
        txActive = false;
    }
    setIntervalFromNow(refresh());
    return 0;
}

int LoRaActivityLED::handleDeepSleep(void *)
{
    sleeping = true;
    refresh();
    return 0;
}

uint32_t LoRaActivityLED::refresh()
{
    uint32_t color = 0;
    uint32_t next = NEOPIXEL_LORA_HEARTBEAT_PERIOD_MS;

    // The TX minimum and the RX flash both end on their own, so drop them first and fall through to what is left.
    if (txHeld && !txActive && Throttle::hasElapsed(txStart, NEOPIXEL_LORA_TX_MIN_MS))
        txHeld = false;
    if (rxActive && Throttle::hasElapsed(rxStart, NEOPIXEL_LORA_RX_FLASH_MS))
        rxActive = false;

    if (sleeping) {
        color = 0;
    } else if (txActive) {
        color = NEOPIXEL_LORA_TX_COLOR; // ends on the TX-done notification
    } else if (txHeld) {
        color = NEOPIXEL_LORA_TX_COLOR;
        next = Throttle::remainingMs(txStart, NEOPIXEL_LORA_TX_MIN_MS);
    } else if (rxActive) {
        color = NEOPIXEL_LORA_RX_COLOR;
        next = Throttle::remainingMs(rxStart, NEOPIXEL_LORA_RX_FLASH_MS);
    } else if (!config.device.led_heartbeat_disabled) {
        // Free-running phase, so the blink keeps its rhythm across activity
        const uint32_t phase = millis() % NEOPIXEL_LORA_HEARTBEAT_PERIOD_MS;
        if (phase < NEOPIXEL_LORA_HEARTBEAT_ON_MS) {
            color = NEOPIXEL_LORA_HEARTBEAT_COLOR;
            next = NEOPIXEL_LORA_HEARTBEAT_ON_MS - phase;
        } else {
            next = NEOPIXEL_LORA_HEARTBEAT_PERIOD_MS - phase;
        }
    }

    if (color != shownColor) {
        pixel.setPixelColor(0, color);
        pixel.show();
        shownColor = color;
    }
    return next ? next : 1;
}

int32_t LoRaActivityLED::runOnce()
{
    if (sleeping)
        return disable();
    return refresh();
}

#endif
