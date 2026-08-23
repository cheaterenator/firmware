#include "RemoteHardwareModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Router.h"
#include "configuration.h"
#include "gps/RTC.h"
#include "main.h"
#include <Throttle.h>

#ifdef MESHTASTIC_REMOTE_HARDWARE_PERSIST_GPIO
#include "FSCommon.h"
#include "SPILock.h"
#include "SafeFile.h"
#endif

#define NUM_GPIOS 64

// Because (FIXME) we currently don't tell API clients status on sent messages
// we need to throttle our sending, so that if a gpio is bouncing up and down we
// don't generate more messages than the net can send. So we limit watch messages to
// a max of one change per 30 seconds
#define WATCH_INTERVAL_MSEC (30 * 1000)

// Tests for access to read from or write to a specified GPIO pin
static bool pinAccessAllowed(uint64_t mask, uint8_t pin)
{
    // If undefined pin access is allowed, don't check the pin and just return true
    if (moduleConfig.remote_hardware.allow_undefined_pin_access) {
        return true;
    }

    // Test to see if the pin is in the list of allowed pins and return true if found
    if (mask & (1ULL << pin)) {
        return true;
    }

    return false;
}

/// Set pin modes for every set bit in a mask
static void pinModes(uint64_t mask, uint8_t mode, uint64_t maskAvailable)
{
    for (uint64_t i = 0; i < NUM_GPIOS; i++) {
        if (mask & (1ULL << i)) {
            if (pinAccessAllowed(maskAvailable, i)) {
                pinMode(i, mode);
            }
        }
    }
}

/// Read all the pins mentioned in a mask
static uint64_t digitalReads(uint64_t mask, uint64_t maskAvailable)
{
    uint64_t res = 0;

    pinModes(mask, INPUT_PULLUP, maskAvailable);

    for (uint64_t i = 0; i < NUM_GPIOS; i++) {
        uint64_t m = 1ULL << i;
        if (mask & m && pinAccessAllowed(maskAvailable, i)) {
            if (digitalRead(i)) {
                res |= m;
            }
        }
    }

    return res;
}

RemoteHardwareModule::RemoteHardwareModule()
    : ProtobufModule("remotehardware", meshtastic_PortNum_REMOTE_HARDWARE_APP, &meshtastic_HardwareMessage_msg),
      concurrency::OSThread("RemoteHardware")
{
    // restrict to the gpio channel for rx
    boundChannel = Channels::gpioChannel;

    // Pull available pin allowlist from config and build a bitmask out of it for fast comparisons later
    for (uint8_t i = 0; i < 4; i++) {
        availablePins += 1ULL << moduleConfig.remote_hardware.available_pins[i].gpio_pin;
    }

#ifdef MESHTASTIC_REMOTE_HARDWARE_PERSIST_GPIO
    if (moduleConfig.remote_hardware.enabled)
        loadGpioState();
#endif
}

bool RemoteHardwareModule::handleReceivedProtobuf(const meshtastic_MeshPacket &req, meshtastic_HardwareMessage *pptr)
{
    if (moduleConfig.remote_hardware.enabled) {
        auto p = *pptr;
        LOG_INFO("Received RemoteHardware type=%d", p.type);

        switch (p.type) {
        case meshtastic_HardwareMessage_Type_WRITE_GPIOS: {
            pinModes(p.gpio_mask, OUTPUT, availablePins);
            for (uint8_t i = 0; i < NUM_GPIOS; i++) {
                uint64_t mask = 1ULL << i;
                if (p.gpio_mask & mask && pinAccessAllowed(availablePins, i)) {
                    digitalWrite(i, (p.gpio_value & mask) ? 1 : 0);
                }
            }

#ifdef MESHTASTIC_REMOTE_HARDWARE_PERSIST_GPIO
            persistGpioWrite(p.gpio_mask, p.gpio_value);
#endif

            break;
        }

        case meshtastic_HardwareMessage_Type_READ_GPIOS: {
            uint64_t res = digitalReads(p.gpio_mask, availablePins);

            // Send the reply
            meshtastic_HardwareMessage r = meshtastic_HardwareMessage_init_default;
            r.type = meshtastic_HardwareMessage_Type_READ_GPIOS_REPLY;
            r.gpio_value = res;
            r.gpio_mask = p.gpio_mask;
            meshtastic_MeshPacket *p2 = allocDataProtobuf(r);
            if (p2) {
                setReplyTo(p2, req);
                myReply = p2;
            }
            break;
        }

        case meshtastic_HardwareMessage_Type_WATCH_GPIOS: {
            watchGpios = p.gpio_mask;
            lastWatchMsec = 0; // Force a new publish soon
            previousWatch =
                ~watchGpios;   // generate a 'previous' value which is guaranteed to not match (to force an initial publish)
            enabled = true;    // Let our thread run at least once
            setInterval(2000); // Set a new interval so we'll run soon
            LOG_INFO("Now watching GPIOs 0x%llx", watchGpios);
            break;
        }

        case meshtastic_HardwareMessage_Type_READ_GPIOS_REPLY:
        case meshtastic_HardwareMessage_Type_GPIOS_CHANGED:
            break; // Ignore - we might see our own replies

        default:
            LOG_ERROR("Hardware operation %d not yet implemented! FIXME", p.type);
            break;
        }
    }

    return false;
}

int32_t RemoteHardwareModule::runOnce()
{
    if (moduleConfig.remote_hardware.enabled && watchGpios) {

        if (!Throttle::isWithinTimespanMs(lastWatchMsec, WATCH_INTERVAL_MSEC)) {
            uint64_t curVal = digitalReads(watchGpios, availablePins);
            lastWatchMsec = millis();

            if (curVal != previousWatch) {
                previousWatch = curVal;
                LOG_INFO("Broadcast GPIOS 0x%llx changed", curVal);

                // Something changed!  Tell the world with a broadcast message
                meshtastic_HardwareMessage r = meshtastic_HardwareMessage_init_default;
                r.type = meshtastic_HardwareMessage_Type_GPIOS_CHANGED;
                r.gpio_value = curVal;
                meshtastic_MeshPacket *p = allocDataProtobuf(r);
                if (p)
                    service->sendToMesh(p);
            }
        }
    } else {
        // No longer watching anything - stop using CPU
        return disable();
    }

    return 2000; // Poll our GPIOs every 2000ms
}

#ifdef MESHTASTIC_REMOTE_HARDWARE_PERSIST_GPIO

// Not every arch has a real filesystem; skip persistence there rather than declaring dead constants.
#ifdef FSCom
namespace
{
const char *const gpioStateFile = "/prefs/remotehw_gpio.dat";
constexpr uint8_t GPIO_PERSIST_VERSION = 1;

struct GpioPersistData {
    uint64_t mask;
    uint64_t value;
};

// Lightweight corruption check, not a security control - matches the XOR-style checksums used elsewhere for prefs files.
uint64_t gpioStateChecksum(const GpioPersistData &d)
{
    return d.mask ^ d.value ^ 0x475049004F5347ULL;
}
} // namespace
#endif

// Restore any persisted output pins at boot, so a relay wired to this device doesn't reset on reboot/power loss.
// Only pins still within the current available_pins allowlist are restored - config may have changed since the last save.
void RemoteHardwareModule::loadGpioState()
{
#ifdef FSCom
    concurrency::LockGuard g(spiLock);

    auto f = FSCom.open(gpioStateFile, FILE_O_READ);
    if (!f) {
        LOG_INFO("RemoteHardware: no saved GPIO state, starting fresh");
        return;
    }

    uint8_t version = 0;
    GpioPersistData data{};
    uint64_t savedChecksum = 0;
    bool ok = f.read(&version, sizeof(version)) == sizeof(version) && version == GPIO_PERSIST_VERSION &&
              f.read((uint8_t *)&data, sizeof(data)) == sizeof(data) &&
              f.read((uint8_t *)&savedChecksum, sizeof(savedChecksum)) == sizeof(savedChecksum);
    f.close();

    if (!ok || savedChecksum != gpioStateChecksum(data)) {
        LOG_WARN("RemoteHardware: saved GPIO state missing or corrupt, ignoring");
        return;
    }

    for (uint8_t i = 0; i < NUM_GPIOS; i++) {
        uint64_t mask = 1ULL << i;
        if ((data.mask & mask) && pinAccessAllowed(availablePins, i)) {
            pinMode(i, OUTPUT);
            digitalWrite(i, (data.value & mask) ? 1 : 0);
            persistedMask |= mask;
            persistedValue |= (data.value & mask);
        }
    }
    LOG_INFO("RemoteHardware: restored GPIO state mask=0x%llx value=0x%llx", persistedMask, persistedValue);
#endif
}

// Merge a WRITE_GPIOS command into our persisted state, restricted to pins we're actually allowed to drive.
// Skips the flash write entirely when nothing allowed actually changed.
void RemoteHardwareModule::persistGpioWrite(uint64_t mask, uint64_t value)
{
#ifdef FSCom
    uint64_t allowedMask = 0;
    for (uint8_t i = 0; i < NUM_GPIOS; i++) {
        uint64_t m = 1ULL << i;
        if ((mask & m) && pinAccessAllowed(availablePins, i))
            allowedMask |= m;
    }
    if (!allowedMask)
        return;

    uint64_t newMask = persistedMask | allowedMask;
    uint64_t newValue = (persistedValue & ~allowedMask) | (value & allowedMask);
    if (newMask == persistedMask && newValue == persistedValue)
        return; // Nothing actually changed - don't wear the flash for a no-op write

    persistedMask = newMask;
    persistedValue = newValue;
    saveGpioState();
#endif
}

// Atomic write (temp file + rename) so a power loss mid-write can't leave a torn file behind -
// the same failure this feature exists to recover from.
void RemoteHardwareModule::saveGpioState()
{
#ifdef FSCom
    spiLock->lock();
    FSCom.mkdir("/prefs");
    spiLock->unlock();

    GpioPersistData data{persistedMask, persistedValue};
    uint64_t checksum = gpioStateChecksum(data);
    uint8_t version = GPIO_PERSIST_VERSION;

    auto f = SafeFile(gpioStateFile, true);
    spiLock->lock();
    f.write(version);
    f.write((const uint8_t *)&data, sizeof(data));
    f.write((const uint8_t *)&checksum, sizeof(checksum));
    spiLock->unlock();

    if (!f.close())
        LOG_ERROR("RemoteHardware: failed to save GPIO state");
    else
        LOG_INFO("RemoteHardware: saved GPIO state mask=0x%llx value=0x%llx", persistedMask, persistedValue);
#endif
}

#endif // MESHTASTIC_REMOTE_HARDWARE_PERSIST_GPIO
