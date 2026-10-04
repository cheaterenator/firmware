"""
PlatformIO extra_script - dodaje custom target "ota_full", ktory po zwyklym
buildzie skleja firmware-*.factory.bin + mt-<mcu>-ota.bin + littlefs-*.bin
w jeden obraz "release/*-full.bin", wywoluj Build-OtaFactoryImage.ps1.

Uzycie w platformio.ini (dopisz linie post:, nie zastepuj istniejacych extra_scripts):

    [env:twoj_env]
    extra_scripts =
        pre:bin/platformio-custom.py
        post:bin/ota_full_image.py

Wymaga:
    - bin/Build-OtaFactoryImage.ps1  (skrypt, ktory sklada obraz)
    - release/*.mt.json, release/*.factory.bin, release/littlefs-*.bin
      (standardowy wynik zwyklego builda/release)
    - release/mt-<mcu>-ota.bin       (pobrany raz z
      https://github.com/meshtastic/esp32-unified-ota/releases i wrzucony do release/)

Odpalenie:
    - VSCode: panel PlatformIO -> Project Tasks -> <env> -> Custom -> "OTA: full image"
    - CLI:    pio run -e <env> -t ota_full
"""

Import("env")

import subprocess
from pathlib import Path


def build_ota_full_image(source, target, env):
    project_dir = Path(env["PROJECT_DIR"])
    release_dir = project_dir / "release"
    ps_script = project_dir / "bin" / "Build-OtaFactoryImage.ps1"

    if not ps_script.exists():
        print(f"[ota_full] Nie znaleziono {ps_script}")
        print("[ota_full] Wrzuc tam Build-OtaFactoryImage.ps1 albo popraw sciezke w bin/ota_full_image.py")
        return

    if not release_dir.exists():
        print(f"[ota_full] Brak katalogu {release_dir} - najpierw zrob zwykly Build (release).")
        return

    # Rozmiar flasha z definicji plytki, np. "4MB" / "8MB" / "16MB" -> podamy jako -FlashSize,
    # zeby wynikowy obraz mial dokladnie rozmiar chipa (esptool --fill-flash-size).
    flash_size = None
    try:
        board = env.BoardConfig()
        raw = board.get("upload.flash_size", None)
        if raw:
            flash_size = raw.upper()
            if not flash_size.endswith("MB") and not flash_size.endswith("KB"):
                flash_size = f"{flash_size}MB"
    except Exception as exc:
        print(f"[ota_full] Nie udalo sie odczytac flash_size z konfiguracji plytki: {exc}")

    cmd = [
        "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", str(ps_script),
        "-Directory", str(release_dir),
    ]
    if flash_size:
        cmd += ["-FlashSize", flash_size]

    print("[ota_full] " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
    result = subprocess.run(cmd)
    if result.returncode != 0:
        print(f"[ota_full] Build-OtaFactoryImage.ps1 zakonczyl sie kodem {result.returncode}")


env.AddCustomTarget(
    name="ota_full",
    dependencies=None,
    actions=[build_ota_full_image],
    title="OTA: full image",
    description="Sklej factory + esp32-unified-ota + littlefs w jeden obraz release/*-full.bin",
)
