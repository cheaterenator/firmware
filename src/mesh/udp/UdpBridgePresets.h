#pragma once
// Compile-time preset list for the UDP default-channel bridge.
//
// What this is: two (or more) physically separate LoRa meshes - different regions/frequencies and/or
// different modem presets, e.g. a 433 MHz LongFast mesh and an 868 MHz NarrowFast mesh - that can't
// hear each other over RF. Nodes from both meshes that also share a local IP network (Wi-Fi/Ethernet)
// can bridge their default channel across that boundary over UDP multicast (see UdpMulticastHandler.h).
//
// This is OFF by default. Enable it per board by adding to that variant's platformio.ini:
//   -D UDP_PRESET_BRIDGE=1
// which also requires the board to build with HAS_UDP_MULTICAST=1 (most ESP32 variants already do,
// see esp32-common.ini) and, at runtime, config.network.enabled_protocols to have UDP_BROADCAST set.
//
// List every preset that should be allowed to bridge below. Only the default/preset-named channel is
// affected - a custom-named channel's hash never depends on the local modem preset, so it already
// bridges transparently over UDP without any of this. Region does not matter here either: only the
// preset's display name and the public default PSK feed the channel hash.
#if defined(UDP_PRESET_BRIDGE) && UDP_PRESET_BRIDGE && !HAS_UDP_MULTICAST
#error "UDP_PRESET_BRIDGE=1 requires a board built with HAS_UDP_MULTICAST=1 (see esp32-common.ini)"
#endif

#if HAS_UDP_MULTICAST && defined(UDP_PRESET_BRIDGE) && UDP_PRESET_BRIDGE

#ifndef UDP_BRIDGE_PRESET_LIST
// Default pairing: 433 MHz LongFast <-> 868/915 MHz NarrowFast. Edit this list (or override
// UDP_BRIDGE_PRESET_LIST with a -D build flag) to match your own deployment.
#define UDP_BRIDGE_PRESET_LIST                                                                                                 \
    {meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST, meshtastic_Config_LoRaConfig_ModemPreset_NARROW_FAST}
#endif

static const meshtastic_Config_LoRaConfig_ModemPreset udpBridgePresets[] = UDP_BRIDGE_PRESET_LIST;
static const size_t udpBridgePresetsCount = sizeof(udpBridgePresets) / sizeof(udpBridgePresets[0]);

#endif // HAS_UDP_MULTICAST && UDP_PRESET_BRIDGE
