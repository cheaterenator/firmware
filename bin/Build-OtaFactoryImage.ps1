<#
.SYNOPSIS
    Skleja firmware-*.factory.bin + mt-<mcu>-ota.bin + littlefs-*.bin w jeden obraz "full",
    gotowy do wgrania od adresu 0x0, na podstawie offsetow partycji z manifestu *.mt.json.

.DESCRIPTION
    Uruchom skrypt z katalogu, w ktorym leza pliki z builda:
        firmware-<env>-<ver>.factory.bin
        firmware-<env>-<ver>.mt.json
        littlefs-<env>-<ver>.bin
        mt-<mcu>-ota.bin      (pobrany z https://github.com/meshtastic/esp32-unified-ota/releases)

    Skrypt czyta pole "mcu" oraz offsety partycji "ota_1" i "spiffs" bezposrednio z manifestu
    (natywny ConvertFrom-Json - bez jq), po czym woła "esptool merge_bin", uzywajac
    --flash_mode/--flash_freq/--flash_size keep, zeby nie ruszac naglowkow zadnego z obrazow.
    Puste obszary (nvs, otadata) esptool sam wypelnia bajtami 0xFF.

    Ten sam mechanizm (ConvertFrom-Json + filtrowanie po "subtype") uzywa oficjalny
    bin/device-install.bat z repo meshtastic/firmware, wiec nazwy pol sa zgodne z tym,
    co juz generuje wasz build (bin/platformio-custom.py).

.PARAMETER Directory
    Katalog z plikami builda. Domyslnie biezacy katalog.

.PARAMETER ManifestPath
    Konkretny plik *.mt.json, jesli w katalogu jest ich wiecej niz jeden.

.PARAMETER OtaBinPath
    Konkretny plik mt-<mcu>-ota.bin, jesli nie lezy obok reszty albo ma inna nazwe.

.PARAMETER LittlefsBinPath
    Konkretny plik littlefs-*.bin, jesli automatyczne wykrywanie (rowniez z dowolnym
    przedrostkiem przed "littlefs-", np. "MT_SW_littlefs-...") go nie znajdzie.

.PARAMETER OutputPath
    Nazwa pliku wynikowego. Domyslnie "<nazwa-manifestu-bez-.mt.json>-full.bin".

.PARAMETER FlashSize
    Realny rozmiar flasha plytki (np. 4MB, 8MB, 16MB). Jesli podany, skrypt doda
    --fill-flash-size, zeby wynikowy plik mial dokladnie rozmiar calego chipa i mozna go
    bylo wgrac od 0x0 bez wczesniejszego erase_flash. Bez tego parametru plik konczy sie
    na koncu partycji spiffs (tak jak w przykladzie z esptoolem --flash_size keep).

.PARAMETER EsptoolCommand
    Wymus konkretna komende esptool (np. pelna sciezke do esptool.exe). Domyslnie skrypt
    sam szuka: esptool -> esptool.py -> py -3 -m esptool -> python -m esptool -> python3 -m esptool.

.PARAMETER DryRun
    Pokaz tylko komende, ktora zostalaby wykonana, bez faktycznego wywolania esptoola.

.EXAMPLE
    cd D:\meshtastic\release
    .\Build-OtaFactoryImage.ps1

.EXAMPLE
    .\Build-OtaFactoryImage.ps1 -FlashSize 8MB
#>

[CmdletBinding()]
param(
    [string]$Directory = (Get-Location).Path,
    [string]$ManifestPath,
    [string]$OtaBinPath,
    [string]$LittlefsBinPath,
    [string]$OutputPath,
    [string]$FlashSize,
    [string]$EsptoolCommand,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

function ConvertTo-HexOffset {
    param($Value)
    if ($null -eq $Value -or $Value -eq '') { return $null }
    if ($Value -is [string] -and $Value -match '^0[xX][0-9a-fA-F]+$') {
        return $Value.ToLower()
    }
    $num = [int64]$Value
    return ('0x{0:x}' -f $num)
}

function ConvertTo-Int64Offset {
    param([string]$HexOffset)
    $clean = $HexOffset -replace '^0[xX]', ''
    return [Convert]::ToInt64($clean, 16)
}

function Resolve-Esptool {
    param([string]$Override)
    if ($Override) {
        return [pscustomobject]@{ Exe = $Override; BaseArgs = @() }
    }
    if (Get-Command esptool -ErrorAction SilentlyContinue)    { return [pscustomobject]@{ Exe = 'esptool';    BaseArgs = @() } }
    if (Get-Command esptool.py -ErrorAction SilentlyContinue) { return [pscustomobject]@{ Exe = 'esptool.py'; BaseArgs = @() } }
    if (Get-Command py -ErrorAction SilentlyContinue)         { return [pscustomobject]@{ Exe = 'py';         BaseArgs = @('-3', '-m', 'esptool') } }
    if (Get-Command python -ErrorAction SilentlyContinue)     { return [pscustomobject]@{ Exe = 'python';     BaseArgs = @('-m', 'esptool') } }
    if (Get-Command python3 -ErrorAction SilentlyContinue)    { return [pscustomobject]@{ Exe = 'python3';    BaseArgs = @('-m', 'esptool') } }
    throw "Nie znaleziono ani 'esptool', ani Pythona w PATH. Zainstaluj: pip install esptool"
}

function Test-SegmentFits {
    param([string]$Name, [int64]$Size, [int64]$StartOffset, [int64]$NextOffset)
    $end = $StartOffset + $Size
    if ($end -gt $NextOffset) {
        Write-Warning ("{0} ({1} B) siega poza poczatek kolejnej partycji: koniec 0x{2:x} > 0x{3:x}. Sprawdz obraz / partition table." -f $Name, $Size, $end, $NextOffset)
    }
}

try {
    $Directory = (Resolve-Path -LiteralPath $Directory).Path

    # --- Manifest ---
    if (-not $ManifestPath) {
        $manifestCandidates = @(Get-ChildItem -Path $Directory -Filter '*.mt.json' -File)
        if ($manifestCandidates.Count -eq 0) {
            throw "Nie znaleziono pliku *.mt.json w '$Directory'. Wskaz go przez -ManifestPath."
        }
        if ($manifestCandidates.Count -gt 1) {
            throw "Znaleziono kilka plikow *.mt.json w '$Directory':`n  $($manifestCandidates.Name -join "`n  ")`nWskaz jeden przez -ManifestPath."
        }
        $ManifestPath = $manifestCandidates[0].FullName
    }
    $ManifestPath = (Resolve-Path -LiteralPath $ManifestPath).Path
    $baseName = (Get-Item $ManifestPath).Name -replace '\.mt\.json$', ''

    Write-Host "Manifest:      $(Split-Path $ManifestPath -Leaf)"

    $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json

    $mcu = $manifest.mcu
    if (-not $mcu) { throw "Manifest nie zawiera pola 'mcu'." }

    $otaPart = $null
    $spiffsPart = $null
    if ($manifest.part) {
        $otaPart    = $manifest.part | Where-Object { $_.subtype -eq 'ota_1'  } | Select-Object -First 1
        $spiffsPart = $manifest.part | Where-Object { $_.subtype -eq 'spiffs' } | Select-Object -First 1
    }

    if ($otaPart) {
        $otaOffset = ConvertTo-HexOffset $otaPart.offset
    } else {
        $otaOffset = '0x260000'
        Write-Warning "Manifest nie ma wpisu partycji 'ota_1' - uzywam domyslnego offsetu dla plytki 4MB ($otaOffset). To NIE zadziala poprawnie na 8/16MB - w takim wypadku uzupelnij manifest albo podaj offset recznie."
    }

    if ($spiffsPart) {
        $spiffsOffset = ConvertTo-HexOffset $spiffsPart.offset
    } else {
        $spiffsOffset = '0x300000'
        Write-Warning "Manifest nie ma wpisu partycji 'spiffs' - uzywam domyslnego offsetu dla plytki 4MB ($spiffsOffset). To NIE zadziala poprawnie na 8/16MB - w takim wypadku uzupelnij manifest albo podaj offset recznie."
    }

    Write-Host "MCU:           $mcu"
    Write-Host "Offset ota_1:  $otaOffset"
    Write-Host "Offset spiffs: $spiffsOffset"

    # --- Pliki binarne ---
    # Uwaga: baseName moze miec dowolny przedrostek przed "firmware-" (np. narzucony
    # przez wasz build/CI, jak "MT_SW_firmware-..."), wiec wszystkie dopasowania nizej
    # sa robione na zasadzie podciagu (*wzorzec*), a nie kotwiczenia na poczatku nazwy.

    $factoryBin = Join-Path $Directory "$baseName.factory.bin"
    if (-not (Test-Path -LiteralPath $factoryBin)) {
        $factoryCandidates = @(Get-ChildItem -Path $Directory -Filter '*.factory.bin' -File)
        if ($factoryCandidates.Count -eq 1) {
            $factoryBin = $factoryCandidates[0].FullName
        } elseif ($factoryCandidates.Count -eq 0) {
            throw "Brak pliku '$factoryBin' (i nie znaleziono zadnego *.factory.bin w '$Directory')."
        } else {
            throw "Znaleziono kilka plikow *.factory.bin, nie moge zgadnac ktory pasuje do '$baseName': $($factoryCandidates.Name -join ', ')"
        }
    }

    if (-not $LittlefsBinPath) {
        # Podmien token "firmware-" na "littlefs-" gdziekolwiek wystepuje w nazwie manifestu
        # (bez kotwiczenia na poczatku), np. "MT_SW_firmware-x-1.0.bin" -> "MT_SW_littlefs-x-1.0.bin".
        $littlefsGuess = Join-Path $Directory (($baseName -replace 'firmware-', 'littlefs-') + '.bin')
        if (Test-Path -LiteralPath $littlefsGuess) {
            $LittlefsBinPath = $littlefsGuess
        } else {
            $littlefsCandidates = @(Get-ChildItem -Path $Directory -Filter '*littlefs*.bin' -File)
            if ($littlefsCandidates.Count -eq 1) {
                $LittlefsBinPath = $littlefsCandidates[0].FullName
            } elseif ($littlefsCandidates.Count -eq 0) {
                throw "Nie znaleziono pliku *littlefs*.bin w '$Directory'. Wskaz go przez -LittlefsBinPath."
            } else {
                throw "Znaleziono kilka plikow *littlefs*.bin, nie moge zgadnac wlasciwego dla '$baseName': $($littlefsCandidates.Name -join ', ')"
            }
        }
    }
    $LittlefsBinPath = (Resolve-Path -LiteralPath $LittlefsBinPath).Path

    if (-not $OtaBinPath) {
        $otaGuess = Join-Path $Directory "mt-$mcu-ota.bin"
        if (Test-Path -LiteralPath $otaGuess) {
            $OtaBinPath = $otaGuess
        } else {
            $otaCandidates = @(Get-ChildItem -Path $Directory -Filter '*-ota.bin' -File)
            if ($otaCandidates.Count -eq 1) {
                $OtaBinPath = $otaCandidates[0].FullName
            } elseif ($otaCandidates.Count -eq 0) {
                throw "Nie znaleziono pliku mt-$mcu-ota.bin (ani zadnego *-ota.bin). Pobierz go z https://github.com/meshtastic/esp32-unified-ota/releases i wrzuc do '$Directory' (albo wskaz -OtaBinPath)."
            } else {
                throw "Znaleziono kilka plikow *-ota.bin, wskaz wlasciwy przez -OtaBinPath: $($otaCandidates.Name -join ', ')"
            }
        }
    }
    $OtaBinPath = (Resolve-Path -LiteralPath $OtaBinPath).Path

    if (-not $OutputPath) {
        $OutputPath = Join-Path $Directory "$baseName-full.bin"
    }

    # --- Kolejnosc segmentow wg adresu ---
    $segments = @(
        [pscustomobject]@{ OffsetHex = '0x0';        File = $factoryBin }
        [pscustomobject]@{ OffsetHex = $otaOffset;    File = $OtaBinPath }
        [pscustomobject]@{ OffsetHex = $spiffsOffset; File = $LittlefsBinPath }
    ) | Sort-Object { ConvertTo-Int64Offset $_.OffsetHex }

    Write-Host ""
    Write-Host "Segmenty (rosnaco):"
    foreach ($seg in $segments) {
        $size = (Get-Item -LiteralPath $seg.File).Length
        Write-Host ("  {0,-10} {1,10} B   {2}" -f $seg.OffsetHex, $size, (Split-Path $seg.File -Leaf))
    }

    for ($i = 0; $i -lt $segments.Count - 1; $i++) {
        $cur = $segments[$i]
        $next = $segments[$i + 1]
        Test-SegmentFits -Name (Split-Path $cur.File -Leaf) `
                          -Size (Get-Item -LiteralPath $cur.File).Length `
                          -StartOffset (ConvertTo-Int64Offset $cur.OffsetHex) `
                          -NextOffset  (ConvertTo-Int64Offset $next.OffsetHex)
    }

    $segmentArgs = @()
    foreach ($seg in $segments) { $segmentArgs += @($seg.OffsetHex, $seg.File) }

    # --- Parametry flasha ---
    $flashArgs = @('--flash_mode', 'keep', '--flash_freq', 'keep')
    if ($FlashSize) {
        $flashArgs += @('--flash_size', $FlashSize, '--fill-flash-size', $FlashSize)
    } else {
        $flashArgs += @('--flash_size', 'keep')
    }

    # --- esptool ---
    $esptool = Resolve-Esptool -Override $EsptoolCommand
    $fullArgs = $esptool.BaseArgs + @('--chip', $mcu, 'merge_bin') + $flashArgs + @('-o', $OutputPath) + $segmentArgs

    Write-Host ""
    Write-Host ("> {0} {1}" -f $esptool.Exe, ($fullArgs -join ' '))
    Write-Host ""

    if ($DryRun) {
        Write-Host "[DryRun] Powyzsza komenda NIE zostala wykonana." -ForegroundColor Yellow
        return
    }

    & $esptool.Exe @fullArgs
    if ($LASTEXITCODE -ne 0) {
        throw "esptool zakonczyl sie kodem $LASTEXITCODE"
    }

    Write-Host ""
    Write-Host "Gotowe -> $OutputPath" -ForegroundColor Green
    Write-Host ""
    Write-Host "Flashowanie u kogos innego (od zera, bez erase_flash):"
    Write-Host "  esptool --chip $mcu -p COM3 write_flash 0x0 `"$OutputPath`""
}
catch {
    Write-Host ""
    Write-Host "BLAD: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
