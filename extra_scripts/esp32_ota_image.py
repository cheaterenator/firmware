#!/usr/bin/env python3
# trunk-ignore-all(ruff/F821)
# trunk-ignore-all(flake8/F821): For SConstruct imports
# trunk-ignore-all(ruff/E402): Hacky esptool import
# trunk-ignore-all(flake8/E402): Hacky esptool import
"""Custom targets producing a single ESP32 image with the unified OTA app in ota_1.

ota_download ("OTA: download images"): fetches every mt-<mcu>-ota.bin of the latest
meshtastic/esp32-unified-ota release into <workspace_dir>/ota, shared by all envs.

ota_full ("OTA: full image"): builds the env, then merges factory.bin at 0x0,
mt-<mcu>-ota.bin at ota_1 and the littlefs image at spiffs into
release/<PROGNAME>-full.bin, padded to the board's flash size so it can be written
at 0x0 without an erase. Writing it erases NVS and replaces littlefs, so the node
loses its config, channels and NodeDB.

ota_full_upload ("OTA: full image & upload"): ota_full, then writes that image at 0x0
through the env's upload port.

ota_update ("OTA: update image"): merges boot_app0 at otadata, the firmware at ota_0
and mt-<mcu>-ota.bin at ota_1 into release/<PROGNAME>-update.bin, written at the
otadata offset. NVS and littlefs lie outside it, so the node keeps its config.

ota_update_upload ("OTA: update image & upload"): ota_update, then writes that image
at the otadata offset through the env's upload port.
"""

import json
import os
import re
import shutil
import sys
import urllib.error
import urllib.request
from os.path import exists, getsize, join

from platformio.util import get_serial_ports

Import("env")
platform = env.PioPlatform()

sys.path.append(platform.get_package_dir("tool-esptoolpy"))
import esptool

OTA_RELEASE_API = (
    "https://api.github.com/repos/meshtastic/esp32-unified-ota/releases/latest"
)
OTA_ASSET_RE = re.compile(r"^mt-[a-z0-9]+-ota\.bin$")
OTA_DIR = join(env.subst("$PROJECT_WORKSPACE_DIR"), "ota")
HTTP_HEADERS = {"User-Agent": "meshtastic-firmware-build"}


def parse_size(value):
    # Partition table offsets/sizes: int, "0x..." or "<n>K" / "<n>M"
    if isinstance(value, int):
        return value
    value = str(value).strip()
    if value[-1:].upper() in ("K", "M"):
        return int(value[:-1], 0) * (1024 if value[-1].upper() == "K" else 1024 * 1024)
    return int(value, 0)


def full_image_path(env):
    return join(env.subst("$PROJECT_DIR"), "release", env.subst("${PROGNAME}-full.bin"))


def update_image_path(env):
    return join(
        env.subst("$PROJECT_DIR"), "release", env.subst("${PROGNAME}-update.bin")
    )


def load_layout(env, tag, required):
    # (mcu, partition list, unified OTA image) from the build manifest, or None after reporting why
    with open(env.subst("$BUILD_DIR/${PROGNAME}.mt.json")) as f:
        manifest = json.load(f)
    part_list = manifest.get("part", [])
    subtypes = {p["subtype"] for p in part_list}
    missing = [s for s in required if s not in subtypes]
    if missing:
        sys.stderr.write(
            f"{tag}: partition table of {env['PIOENV']} has no {' or '.join(missing)} partition\n"
        )
        return None

    ota_bin = join(OTA_DIR, f"mt-{manifest['mcu']}-ota.bin")
    if not exists(ota_bin):
        sys.stderr.write(
            f"{tag}: {ota_bin} not found, run the 'OTA: download images' task (-t ota_download)\n"
        )
        return None
    return manifest["mcu"], part_list, ota_bin


def check_segments(segments, tag):
    # Each (partition or None, offset, file) must exist and fit its partition
    for part, offset, path in segments:
        if not path or not exists(path):
            sys.stderr.write(f"{tag}: {path} not found\n")
            return False
        if part and getsize(path) > parse_size(part["size"]):
            sys.stderr.write(
                f"{tag}: {os.path.basename(path)} ({getsize(path)} B) does not fit "
                f"partition {part['name']} ({parse_size(part['size'])} B)\n"
            )
            return False
    return True


def merge_segments(mcu, out, segments, extra_args):
    os.makedirs(os.path.dirname(out), exist_ok=True)
    # Keep the flash parameters esp32_extra.py baked into factory.bin
    cmd = [
        "--chip",
        mcu,
        "merge_bin",
        "-o",
        out,
        "--flash_mode",
        "keep",
        "--flash_freq",
        "keep",
        "--flash_size",
        "keep",
    ] + extra_args

    print("    Offset | File")
    for _, offset, path in segments:
        print(f" - {hex(offset):>8} | {path}")
        cmd += [hex(offset), path]

    esptool.main(cmd)


def http_get(url):
    return urllib.request.urlopen(
        urllib.request.Request(url, headers=HTTP_HEADERS), timeout=60
    )


def download_ota_images(source, target, env):
    try:
        with http_get(OTA_RELEASE_API) as resp:
            release = json.load(resp)
        assets = [a for a in release.get("assets", []) if OTA_ASSET_RE.match(a["name"])]
        if not assets:
            sys.stderr.write(
                f"ota_download: release {release.get('tag_name')} has no mt-*-ota.bin assets\n"
            )
            return 1
        os.makedirs(OTA_DIR, exist_ok=True)
        print(f"ota_download: esp32-unified-ota {release['tag_name']} -> {OTA_DIR}")
        for asset in assets:
            dest = join(OTA_DIR, asset["name"])
            part = dest + ".part"
            with http_get(asset["browser_download_url"]) as resp, open(part, "wb") as f:
                shutil.copyfileobj(resp, f)
            if getsize(part) != asset["size"]:
                os.remove(part)
                sys.stderr.write(
                    f"ota_download: {asset['name']} truncated ({asset['size']} B expected)\n"
                )
                return 1
            os.replace(part, dest)
            print(f"  {asset['name']:<22} {asset['size']:>8} B")
    except (urllib.error.URLError, OSError, ValueError) as exc:
        sys.stderr.write(f"ota_download: {exc}\n")
        return 1
    return 0


def build_full_image(source, target, env):
    layout = load_layout(env, "ota_full", ("ota_1", "spiffs"))
    if not layout:
        return 1
    mcu, part_list, ota_bin = layout
    parts = {p["subtype"]: p for p in part_list}

    # (partition the file must fit, or None for the factory image at 0x0; offset; file)
    segments = [
        (None, 0, env.subst("$BUILD_DIR/${PROGNAME}.factory.bin")),
        (parts["ota_1"], parse_size(parts["ota_1"]["offset"]), ota_bin),
        (
            parts["spiffs"],
            parse_size(parts["spiffs"]["offset"]),
            env.subst("$BUILD_DIR/${ESP32_FS_IMAGE_NAME}.bin"),
        ),
    ]
    if not check_segments(segments, "ota_full"):
        return 1

    out = full_image_path(env)
    flash_size = env.BoardConfig().get("upload.flash_size", None)
    merge_segments(
        mcu, out, segments, ["--fill-flash-size", flash_size] if flash_size else []
    )
    print(f"ota_full: {out}")
    print(
        "ota_full: writing this image erases NVS and littlefs - the node loses its config"
    )
    print(f"Flash with: esptool --chip {mcu} write_flash 0x0 {os.path.basename(out)}")
    return 0


def update_image_offset(env):
    # The update image starts at otadata; read it back from the manifest for the upload target
    with open(env.subst("$BUILD_DIR/${PROGNAME}.mt.json")) as f:
        manifest = json.load(f)
    otadata = next(p for p in manifest.get("part", []) if p["subtype"] == "ota")
    return parse_size(otadata["offset"])


def build_update_image(source, target, env):
    layout = load_layout(env, "ota_update", ("ota", "ota_0", "ota_1"))
    if not layout:
        return 1
    mcu, part_list, ota_bin = layout
    parts = {p["subtype"]: p for p in part_list}
    start = parse_size(parts["ota"]["offset"])
    end = parse_size(parts["ota_1"]["offset"]) + parse_size(parts["ota_1"]["size"])

    # merge_bin fills gaps with 0xFF, so any other partition inside the span would be erased
    covered = {id(parts[s]) for s in ("ota", "ota_0", "ota_1")}
    for p in part_list:
        p_start = parse_size(p["offset"])
        if (
            id(p) not in covered
            and p_start < end
            and p_start + parse_size(p["size"]) > start
        ):
            sys.stderr.write(
                f"ota_update: partition {p['name']} lies between otadata and ota_1 and would be erased\n"
            )
            return 1

    # boot_app0 resets otadata so the bootloader starts ota_0, as the regular upload target does
    boot_app0 = next(
        (
            env.subst(img[1])
            for img in env.get("FLASH_EXTRA_IMAGES", [])
            if parse_size(env.subst(img[0])) == start
        ),
        None,
    )
    if not boot_app0:
        sys.stderr.write(
            f"ota_update: no FLASH_EXTRA_IMAGES entry at otadata ({hex(start)})\n"
        )
        return 1

    segments = [
        (parts["ota"], start, boot_app0),
        (
            parts["ota_0"],
            parse_size(parts["ota_0"]["offset"]),
            env.subst("$BUILD_DIR/${PROGNAME}.bin"),
        ),
        (parts["ota_1"], parse_size(parts["ota_1"]["offset"]), ota_bin),
    ]
    if not check_segments(segments, "ota_update"):
        return 1

    out = update_image_path(env)
    merge_segments(mcu, out, segments, ["--target-offset", hex(start)])
    print(f"ota_update: {out}")
    print(
        f"Flash with: esptool --chip {mcu} write_flash {hex(start)} {os.path.basename(out)}"
    )
    return 0


def write_image(env, offset, path, tag):
    # Same port handling as the platform's BeforeUpload for the regular upload target
    upload = env.BoardConfig().get("upload", {})
    if not env.subst("$UPLOAD_PORT"):
        env.AutodetectUploadPort()
    before_ports = get_serial_ports()
    if upload.get("use_1200bps_touch", False):
        env.TouchSerialPort("$UPLOAD_PORT", 1200)
    if upload.get("wait_for_upload_port", False):
        env.Replace(UPLOAD_PORT=env.WaitForNewSerialPort(before_ports))

    # Flash parameters default to keep, so the merged image is written byte for byte
    try:
        esptool.main(
            [
                "--chip",
                env.subst("$BOARD_MCU"),
                "--port",
                env.subst("$UPLOAD_PORT"),
                "--baud",
                env.subst("$UPLOAD_SPEED"),
                "--before",
                upload.get("before_reset", "default_reset"),
                "--after",
                upload.get("after_reset", "hard_reset"),
                "write_flash",
                hex(offset),
                path,
            ]
        )
    except esptool.FatalError as exc:
        sys.stderr.write(f"{tag}: {exc}\n")
        return 1
    return 0


def upload_full_image(source, target, env):
    return write_image(env, 0, full_image_path(env), "ota_full_upload")


def upload_update_image(source, target, env):
    return write_image(
        env, update_image_offset(env), update_image_path(env), "ota_update_upload"
    )


env.AddCustomTarget(
    name="ota_download",
    dependencies=None,
    actions=[download_ota_images],
    title="OTA: download images",
    description="Download all mt-<mcu>-ota.bin from the latest esp32-unified-ota release",
)

env.AddCustomTarget(
    name="ota_full",
    # mtjson builds firmware + littlefs and writes the manifest with the partition table
    dependencies=["mtjson"],
    actions=[build_full_image],
    title="OTA: full image",
    description="Merge factory + unified OTA + littlefs into release/<PROGNAME>-full.bin (erases config)",
)

env.AddCustomTarget(
    name="ota_full_upload",
    dependencies=["ota_full"],
    actions=[upload_full_image],
    title="OTA: full image & upload",
    description="Build the full OTA image and write it at 0x0 over the upload port (erases config)",
)

env.AddCustomTarget(
    name="ota_update",
    dependencies=["mtjson"],
    actions=[build_update_image],
    title="OTA: update image",
    description="Merge boot_app0 + firmware + unified OTA into release/<PROGNAME>-update.bin (keeps config)",
)

env.AddCustomTarget(
    name="ota_update_upload",
    dependencies=["ota_update"],
    actions=[upload_update_image],
    title="OTA: update image & upload",
    description="Build the update image and write it at the otadata offset over the upload port (keeps config)",
)
