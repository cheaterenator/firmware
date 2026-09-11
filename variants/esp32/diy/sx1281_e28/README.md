# DIY: ESP32 DevKit + EBYTE E28-2G4M (SX1281)

A from-scratch node built from a generic ESP32-WROOM-32 DevKit and an EBYTE
E28-2G4M12S / E28-2G4M20S / E28-2G4M27S breakout (Semtech **SX1281**,
2.4 GHz LoRa/FLRC/GFSK). Nothing in this repo shipped a variant for the
SX1281 before - this one was created by copying the existing DIY-board
pattern (see `variants/esp32/diy/v1` and `.../hydra`) and swapping in the
SX128x radio family used by `variants/esp32/betafpv_2400_tx_micro`.

## Why this needed a new variant

The only board already in this tree that talks to an SX128x-family radio is
`betafpv_2400_tx_micro` - a ready-made ELRS TX module (ESP32 + **SX1280**)
you just flash, no soldering. If you're open to buying that instead of
building from parts, it's genuinely the easiest path to a 2.4 GHz Meshtastic
node with this firmware. This variant is for when you specifically want (or
already have) a bare SX1281 module wired up yourself.

**SX1281 vs SX1280:** they're register/pin-compatible siblings. The SX1281
just leaves out the ranging engine, which Meshtastic doesn't use anyway.
Meshtastic's driver is literally called `SX1280Interface`
(`SX128xInterface<SX1280>`) and there's no separate SX1281 class - `USE_SX1280`
is the correct define for this module too, and no C++ changes were needed,
only this new variant folder.

## Bill of materials

| Part | Notes |
| --- | --- |
| ESP32-WROOM-32 DevKit (30/38-pin "DOIT" style) | Any clone board works; this targets PlatformIO's `esp32doit-devkit-v1` |
| EBYTE E28-2G4M12S, -20S or -27S | Semtech SX1281, 2.4 GHz. Pick 12S/20S unless you've confirmed 27S (≈27 dBm) is legal for you to run - see **RF power & regulatory notes** |
| 2.4 GHz antenna matching the module's connector (IPEX/U.FL or SMA, check your SKU) | **Never power the module without an antenna attached** - it can damage the PA |
| 0.96" SSD1306 I2C OLED (optional) | |
| NEO-6M / NEO-M8N GPS module (optional) | |
| 18650 cell + TP4056-style charge/protection board (optional) | For battery power; add the voltage-divider resistors below if you also want the firmware to read the voltage |
| 2x 100 kΩ resistors (optional) | Battery voltage divider - adjust `ADC_MULTIPLIER` in `variant.h` to match whatever ratio you actually build |
| Perfboard/breadboard, wire | |

## Wiring

The E28-2G4M's pins (from CDEBYTE's datasheet) and where they go on the ESP32:

| E28-2G4M pin | Function | ESP32 GPIO |
| --- | --- | --- |
| VCC | 1.8-3.6 V | 3V3 |
| GND | ground | GND |
| MISO_TX | SPI MISO | GPIO19 |
| MOSI_RX | SPI MOSI | GPIO23 |
| SCK_RTSN | SPI SCK | GPIO18 |
| NSS_CTS | SPI chip-select | GPIO5 |
| RX_EN | LNA enable, active high | GPIO26 |
| TX_EN | PA enable, active high | GPIO27 |
| NRESET | reset, active low (has its own pull-up) | GPIO14 |
| BUSY | status out | GPIO32 |
| DIO1 | IRQ out | GPIO33 |
| DIO2, DIO3 | not used by this design | leave unconnected |

The ESP32's SPI runs at 3.3 V logic, which is inside the module's 1.8-3.6 V
range, so **no level shifter is needed** - EBYTE's own note is that 5 V TTL
(e.g. a classic 5 V Arduino) risks damaging the module, but a 3.3 V ESP32 is
fine directly.

Everything else (OLED, GPS, button, LED, battery sense) is optional and
already broken out in `variant.h`:

| Peripheral | ESP32 GPIO |
| --- | --- |
| OLED SDA / SCL | GPIO21 / GPIO22 |
| GPS RX / TX (ESP32 side) / enable | GPIO16 / GPIO17 / GPIO4 |
| User button | GPIO0 (the DevKit's built-in BOOT button - no wiring needed) |
| Status LED | GPIO2 (the DevKit's built-in LED - no wiring needed) |
| Battery sense | GPIO35 (through your own divider) |

Delete the corresponding lines from `variant.h` for anything you don't wire up.

## Building and flashing

From the root of this firmware checkout, with PlatformIO installed:

```
pio run -e sx1281-e28-diy -t upload
```

(`board_level = extra` and no `custom_meshtastic_hw_model` are used
deliberately, the same way `ai-c3` and `ikoka_s3` do it - this reports as
"private hardware" rather than claiming an official hardware-model ID, which
is the right choice for a one-off DIY build like this.)

## RF power & regulatory notes

The bare SX1281 die tops out around **+12.5 dBm**; the "12/20/27" in the
module's part number is what its onboard PA gets you *after* that, not a
setting you choose over SPI. `SX128X_MAX_POWER` in `variant.h` is left at a
conservative `12` (the chip's native max). Most 2.4 GHz ISM-band rules
(ETSI in the EU included) cap non-frequency-hopping equipment like this
around **20 dBm EIRP**, so:

* the 12S/20S variants land you at or under that even at full tilt;
* the 27S variant can exceed it, and running it wide open is on you to check
  against your local regulations (and against your antenna's gain, which
  adds to EIRP too).

One upside of picking 2.4 GHz over the sub-GHz (868/915 MHz) radios most
Meshtastic nodes use: in the EU, the 868 MHz ISM sub-bands carry duty-cycle
limits (often 1%), while the 2.4 GHz ISM band Meshtastic's SX128x radios use
is power-limited but not duty-cycle-limited - handy if you want a node that
can transmit more often. The trade-off is range: 2.4 GHz LoRa covers
noticeably less distance than 868/915 MHz for the same power, since path
loss rises with frequency and there's more competing traffic (Wi-Fi,
Bluetooth) in that band.
