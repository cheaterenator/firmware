#include "RemoteGpioButtonModule.h"

// This whole translation unit is compiled unconditionally by the build system (it's not skipped just
// because Modules.cpp doesn't #include the header) - so the opt-in flag has to be checked here too,
// not only around the #include/new in Modules.cpp. Every variant that doesn't pass
// -D MESHTASTIC_REMOTE_GPIO_BUTTON=1 (and isn't ESP32) must reduce this file to nothing, RHB_TARGET_NODENUM
// #error included, or their build breaks regardless of whether they want this module at all.
#if MESHTASTIC_REMOTE_GPIO_BUTTON && defined(ARCH_ESP32)

#include "MeshService.h"
#include "NodeDB.h"
#include "configuration.h"
#include "main.h"
#include "mesh/Channels.h"
#include "mesh/generated/meshtastic/remote_hardware.pb.h"
#include <Throttle.h>

RemoteGpioButtonModule *remoteGpioButtonModule;

// ---- Compile-time configuration --------------------------------------------------------------
// Override any of these from build_flags (-D RHB_xxx=...) instead of editing the defaults below,
// if you'd rather keep board-specific wiring and the target node id out of source control.

#ifndef RHB_LOCAL_BUTTON_PIN
#define RHB_LOCAL_BUTTON_PIN 0 // local ESP32 GPIO the physical button is wired to (button to GND, INPUT_PULLUP)
#endif

#ifndef RHB_TARGET_NODENUM
#define RHB_TARGET_NODENUM 0x00000000 // NodeNum of the device whose GPIO should be toggled - REQUIRED, see #error below
#endif

#ifndef RHB_TARGET_GPIO_PIN
#define RHB_TARGET_GPIO_PIN 0 // GPIO pin number to WRITE on the target device's RemoteHardwareModule
#endif

#ifndef RHB_DEBOUNCE_MS
#define RHB_DEBOUNCE_MS 300 // ignore further edges on the local button for this long after an accepted change
#endif

#ifndef RHB_POLL_MS
#define RHB_POLL_MS 25 // how often to sample the local button pin
#endif

#if RHB_TARGET_NODENUM == 0
#error "RemoteGpioButtonModule: set RHB_TARGET_NODENUM to the target node's NodeNum (its !hex id, as a 0x... literal) via build_flags, or edit the default in RemoteGpioButtonModule.cpp"
#endif

RemoteGpioButtonModule::RemoteGpioButtonModule()
    : SinglePortModule("remoteGpioButton", meshtastic_PortNum_REMOTE_HARDWARE_APP), OSThread("RemoteGpioButton")
{
}

int32_t RemoteGpioButtonModule::runOnce()
{
    if (firstRun) {
        firstRun = false;
        pinMode(RHB_LOCAL_BUTTON_PIN, INPUT_PULLUP);
        lastRawLevel = digitalRead(RHB_LOCAL_BUTTON_PIN);
        return RHB_POLL_MS;
    }

    bool rawLevel = digitalRead(RHB_LOCAL_BUTTON_PIN);
    if (rawLevel != lastRawLevel && Throttle::hasElapsed(lastChangeMsec, RHB_DEBOUNCE_MS)) {
        lastRawLevel = rawLevel;
        lastChangeMsec = millis();

        // Active-LOW button on INPUT_PULLUP: flip our latched state on the press edge only, so a
        // single press toggles the remote pin once (bistable), ignoring the release edge.
        if (rawLevel == LOW) {
            outputState = !outputState;
            sendToggle();
        }
    }

    return RHB_POLL_MS;
}

void RemoteGpioButtonModule::sendToggle()
{
    // Find the "gpio" channel by name - RemoteHardwareModule on the receiving end is bound to it for
    // RX, and refusing to fall back to another channel here keeps a misconfigured device from ever
    // sending this on the public channel.
    int8_t chIndex = -1;
    for (uint8_t i = 0; i < channels.getNumChannels(); i++) {
        if (strcasecmp(channels.getName(i), Channels::gpioChannel) == 0) {
            chIndex = i;
            break;
        }
    }
    if (chIndex < 0) {
        LOG_WARN("RemoteGpioButton: no channel named '%s' configured, dropping toggle", Channels::gpioChannel);
        return;
    }

    meshtastic_HardwareMessage m = meshtastic_HardwareMessage_init_default;
    m.type = meshtastic_HardwareMessage_Type_WRITE_GPIOS;
    m.gpio_mask = 1ULL << RHB_TARGET_GPIO_PIN;
    m.gpio_value = outputState ? m.gpio_mask : 0;

    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p)
        return;

    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_HardwareMessage_msg, &m);
    p->to = RHB_TARGET_NODENUM;
    p->channel = chIndex;
    p->want_ack = true;
    p->priority = meshtastic_MeshPacket_Priority_RELIABLE;

    LOG_INFO("RemoteGpioButton: WRITE_GPIOS pin=%d value=%d -> 0x%08x on channel '%s'", RHB_TARGET_GPIO_PIN, outputState,
             RHB_TARGET_NODENUM, Channels::gpioChannel);
    service->sendToMesh(p);
}

#endif // MESHTASTIC_REMOTE_GPIO_BUTTON && ARCH_ESP32
