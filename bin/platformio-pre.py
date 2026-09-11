#!/usr/bin/env python3
# trunk-ignore-all(ruff/F821)
# trunk-ignore-all(flake8/F821): For SConstruct imports
Import("env")
platform = env.PioPlatform()

# Custom prefix stamped onto every build output filename (firmware-*.bin, .elf,
# .factory.bin, .uf2, littlefs-*.bin, ...). Change here to rebrand build artifacts.
OUTPUT_NAME_PREFIX = "MT_SW_"

if platform.name == "native":
    env.Replace(PROGNAME="meshtasticd")
else:
    from readprops import readProps
    prefsLoc = env["PROJECT_DIR"] + "/version.properties"
    verObj = readProps(prefsLoc)
    env.Replace(PROGNAME=f"{OUTPUT_NAME_PREFIX}firmware-{env.get('PIOENV')}-{verObj['long']}")
    env.Replace(ESP32_FS_IMAGE_NAME=f"{OUTPUT_NAME_PREFIX}littlefs-{env.get('PIOENV')}-{verObj['long']}")

# Print the new program name for verification
print(f"PROGNAME: {env.get('PROGNAME')}")
if platform.name == "espressif32":
    print(f"ESP32_FS_IMAGE_NAME: {env.get('ESP32_FS_IMAGE_NAME')}")
