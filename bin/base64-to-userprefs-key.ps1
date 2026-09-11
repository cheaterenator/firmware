<#
.SYNOPSIS
    Converts a base64-encoded key (e.g. a Meshtastic admin/channel PSK public key)
    into the C-array string format expected by userPrefs.jsonc
    (USERPREFS_USE_ADMIN_KEY_n, USERPREFS_CHANNEL_n_PSK, ...).

.DESCRIPTION
    userPrefs.jsonc keys such as USERPREFS_USE_ADMIN_KEY_0 expect their value to be
    a plain string containing a C initializer list of bytes, e.g.:
        "{ 0x4d, 0xbd, 0xb4, ... }"
    This script takes the usual base64 representation of the key (as shown in the
    Meshtastic app/CLI) and emits that string, ready to paste into userPrefs.jsonc.

.PARAMETER Base64
    The base64-encoded key. Standard alphabet (+/) and URL-safe alphabet (-_) are
    both accepted; missing '=' padding is added automatically.

.PARAMETER NoBraces
    Emit only the comma-separated byte list, without the surrounding "{ }".

.EXAMPLE
    ./bin/base64-to-userprefs-key.ps1 -Base64 "Tb20KZFRUQE10vGxlVEYRpNlfS0cSUJRMHPFgQAo7QU="
	powershell .\base64-to-userprefs-key.ps1 -Base64 "Tb20KZFRUQE10vGxlVEYRpNlfS0cSUJRMHPFgQAo7QU="

.EXAMPLE
    "Tb20KZFRUQE10vGxlVEYRpNlfS0cSUJRMHPFgQAo7QU=" | ./bin/base64-to-userprefs-key.ps1

.EXAMPLE
    # Straight into the clipboard, ready to paste as a userPrefs.jsonc value
    ./bin/base64-to-userprefs-key.ps1 -Base64 "Tb20KZFRUQE10vGxlVEYRpNlfS0cSUJRMHPFgQAo7QU=" | Set-Clipboard
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0, ValueFromPipeline = $true)]
    [string]$Base64,

    [switch]$NoBraces
)

process {
    $b64 = $Base64.Trim()

    # Accept URL-safe base64 (-_) as well as standard (+/).
    $b64 = $b64.Replace('-', '+').Replace('_', '/')

    # Pad to a multiple of 4 if the '=' padding was stripped.
    $remainder = $b64.Length % 4
    if ($remainder -ne 0) {
        $b64 += ('=' * (4 - $remainder))
    }

    try {
        $bytes = [System.Convert]::FromBase64String($b64)
    } catch {
        throw "'$Base64' is not valid base64: $($_.Exception.Message)"
    }

    if ($bytes.Length -eq 0) {
        throw "Decoded 0 bytes from input - nothing to emit."
    }

    $hex = $bytes | ForEach-Object { '0x{0:x2}' -f $_ }
    $list = $hex -join ', '

    if ($NoBraces) {
        $list
    } else {
        "{ $list }"
    }

    Write-Verbose "Decoded $($bytes.Length) bytes from base64 input."
}
