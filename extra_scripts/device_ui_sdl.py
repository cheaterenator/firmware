#!/usr/bin/env python3
# trunk-ignore-all(ruff/F821)
# trunk-ignore-all(flake8/F821): For SConstruct imports
#
# PROTOTYPE for [env:native-windows-mui]: device-ui has X11 and framebuffer display
# drivers only, so this gives it an SDL2 one, edited into the installed library copy
# under .pio/libdeps before the build. LVGL already ships the SDL driver
# (lv_sdl_window_create); X11Driver gains an `#ifdef USE_SDL` branch that uses it.
# The real home for this is meshtastic/device-ui.
#
# Each edit is skipped when its result is already there and fails loudly when neither
# side matches, i.e. after the device-ui pin moved and the edits need redoing.
import os

Import("env")

EDITS = [
    (
        "include/lv_conf.h",
        "#define LV_USE_SDL              0\n",
        "#ifdef USE_SDL\n"
        "#define LV_USE_SDL              1\n"
        "#else\n"
        "#define LV_USE_SDL              0\n"
        "#endif\n",
    ),
    (
        "source/graphics/driver/DisplayDriverFactory.cpp",
        "#if defined(USE_X11)\n",
        "#if defined(USE_X11) || defined(USE_SDL)\n",
    ),
    (
        "source/graphics/driver/DisplayDriverFactory.cpp",
        "#elif defined(USE_X11)\n",
        "#elif defined(USE_X11) || defined(USE_SDL)\n",
    ),
    (
        "source/graphics/driver/X11Driver.cpp",
        "#ifdef USE_X11\n",
        "#if defined(USE_X11) || defined(USE_SDL)\n",
    ),
    (
        "source/graphics/driver/X11Driver.cpp",
        "    display = lv_x11_window_create(title, screenWidth, screenHeight);\n"
        "    lv_x11_inputs_create(display, &mouse_cursor_icon);\n",
        "#ifdef USE_SDL\n"
        "    display = lv_sdl_window_create(screenWidth, screenHeight);\n"
        "    lv_sdl_window_set_title(display, title);\n"
        "    lv_sdl_mouse_create();\n"
        "    lv_sdl_mousewheel_create();\n"
        "    // device-ui puts only the keyboards it drives itself (I2C, libinput) in a group, and a\n"
        "    // keypad without one goes nowhere. Create the default group before the screens are\n"
        "    // built so their widgets join it.\n"
        "    lv_group_t *group = lv_group_get_default();\n"
        "    if (!group) {\n"
        "        group = lv_group_create();\n"
        "        lv_group_set_default(group);\n"
        "    }\n"
        "    lv_indev_set_group(lv_sdl_keyboard_create(), group);\n"
        "#else\n"
        "    display = lv_x11_window_create(title, screenWidth, screenHeight);\n"
        "    lv_x11_inputs_create(display, &mouse_cursor_icon);\n"
        "#endif\n",
    ),
]


def patch_device_ui(lib_dir):
    for rel_path, old, new in EDITS:
        path = os.path.join(lib_dir, rel_path)
        with open(path, encoding="utf-8", newline="") as f:
            raw = f.read()
        # device-ui ships CRLF sources: match on LF, write back in the file's own style.
        text = raw.replace("\r\n", "\n")
        if new in text:
            continue
        if old not in text:
            raise RuntimeError(
                "device_ui_sdl.py: %s no longer contains the text it edits; "
                "redo the SDL edits for this device-ui version" % path
            )
        text = text.replace(old, new)
        if "\r\n" in raw:
            text = text.replace("\n", "\r\n")
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        print("device_ui_sdl.py: patched %s" % rel_path)


lib_dir = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env["PIOENV"], "meshtastic-device-ui")
if os.path.isdir(lib_dir):
    patch_device_ui(lib_dir)
else:
    print("device_ui_sdl.py: %s not installed yet, nothing to patch" % lib_dir)

# 63 `(unsigned long)pointer` casts in device-ui's TFTView_320x240.cpp are errors on LLP64
# Windows, where long is 32 bits. The values they carry fit in 32 bits, so -fpermissive
# (C++ only, hence CXXFLAGS) is enough for the prototype; the fix is uintptr_t upstream.
env.Append(CXXFLAGS=["-fpermissive"])
