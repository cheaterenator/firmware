#pragma once
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"

/**
 * Watches a local, bistable pushbutton and relays its toggled state to a single remote node's GPIO
 * over the "gpio" channel, using the same WRITE_GPIOS wire format RemoteHardwareModule already
 * understands on the receiving end (see RemoteHardwareModule.cpp on the target device).
 *
 * This module never receives - it only transmits WRITE_GPIOS commands - so it is safe to run
 * alongside a RemoteHardwareModule instance (local or remote) without either interfering with the
 * other.
 *
 * Enable with build_flags: -D MESHTASTIC_REMOTE_GPIO_BUTTON=1
 * Configure the local button pin, target node and target pin with the RHB_* defines at the top of
 * RemoteGpioButtonModule.cpp - either edit the defaults there or override them from build_flags.
 *
 * ESP32 only: gated out on other architectures, which don't have the flash/RAM budget to spare.
 */
class RemoteGpioButtonModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    RemoteGpioButtonModule();

  protected:
    /// We only ever send WRITE_GPIOS commands, never receive them.
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override { return false; }

    virtual int32_t runOnce() override;

  private:
    bool firstRun = true;
    bool lastRawLevel = true;    // last debounced raw level of the local button pin (HIGH==1); set for real on first runOnce()
    uint32_t lastChangeMsec = 0; // for debounce
    bool outputState = false;    // the bistable state we last commanded on the remote pin

    void sendToggle();
};

extern RemoteGpioButtonModule *remoteGpioButtonModule;
