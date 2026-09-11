#pragma once
#include "ProtobufModule.h"
#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/remote_hardware.pb.h"

/**
 * A module that provides easy low-level remote access to device hardware.
 */
class RemoteHardwareModule : public ProtobufModule<meshtastic_HardwareMessage>, private concurrency::OSThread
{
    /// The current set of GPIOs we've been asked to watch for changes
    uint64_t watchGpios = 0;

    /// The previously read value of watched pins
    uint64_t previousWatch = 0;

    /// The timestamp of our last watch event (we throttle watches to 1 change every 30 seconds)
    uint32_t lastWatchMsec = 0;

    /// A bitmask of GPIOs that are exposed to the mesh if undefined access is not enabled
    uint64_t availablePins = 0;

#ifdef MESHTASTIC_REMOTE_HARDWARE_PERSIST_GPIO
    /// GPIOs we've been told to WRITE at least once and their last commanded value, persisted across reboots
    uint64_t persistedMask = 0;
    uint64_t persistedValue = 0;

    /// Restore any persisted output pins at boot
    void loadGpioState();

    /// Merge a WRITE_GPIOS command into our persisted state and flush to flash if it changed
    void persistGpioWrite(uint64_t mask, uint64_t value);

    /// Write persistedMask/persistedValue to flash
    void saveGpioState();
#endif

  public:
    /** Constructor
     * name is for debugging output
     */
    RemoteHardwareModule();

  protected:
    /** Called to handle a particular incoming message

    @return true if you've guaranteed you've handled this message and no other handlers should be considered for it
    */
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_HardwareMessage *p) override;

    /**
     * Periodically read the gpios we have been asked to WATCH, if they have changed,
     * broadcast a message with the change information.
     *
     * The method that will be called each time our thread gets a chance to run
     *
     * Returns desired period for next invocation (or RUN_SAME for no change)
     */
    virtual int32_t runOnce() override;
};

extern RemoteHardwareModule remoteHardwareModule;
