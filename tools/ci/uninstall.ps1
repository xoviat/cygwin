#Requires -Version 5.1
<#
.SYNOPSIS
  Revert everything install.ps1 did to a Git for Windows install.

.DESCRIPTION
  Removes the installed msys-2.0.dll and renames the msys-2.0.dll.stock
  backup(s) back into place, then restores the PE flags that install.ps1
  changed (recorded in usr\bin\msys-2.0.dll.dropin.json) and removes the
  manifest.  Only the changes install.ps1 made are undone.

.PARAMETER GitRoot
  Root of the Git for Windows install.  Default: "C:\Program Files\Git".

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File uninstall.ps1
#>
[CmdletBinding()]
param(
    [string]$GitRoot = "C:\Program Files\Git"
)

$ErrorActionPreference = "Stop"

if (!(Test-Path (Join-Path $GitRoot "usr\bin\bash.exe"))) {
    throw "Not a Git for Windows install: $GitRoot"
}

# --- 1. restore the original runtime DLLs --------------------------------

foreach ($rel in @("usr\bin", "bin")) {
    $dir = Join-Path $GitRoot $rel
    if (!(Test-Path -LiteralPath $dir)) { continue }
    $target = Join-Path $dir "msys-2.0.dll"
    $backup = Join-Path $dir "msys-2.0.dll.stock"
    if (Test-Path -LiteralPath $backup) {
        if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force }
        Move-Item -LiteralPath $backup -Destination $target
        Write-Host "restored: $rel\msys-2.0.dll"
    } elseif (Test-Path -LiteralPath $target) {
        Write-Host "no backup ($rel\msys-2.0.dll.stock); leaving $rel\msys-2.0.dll in place"
    }
}

# --- 2. restore the PE flags recorded by install.ps1 ---------------------

$manifestPath = Join-Path (Join-Path $GitRoot "usr\bin") "msys-2.0.dll.dropin.json"

if (Test-Path -LiteralPath $manifestPath) {
    $entries = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($null -eq $entries) { $entries = @() }
    if ($entries -isnot [System.Array]) { $entries = @($entries) }
    foreach ($e in $entries) {
        $p = Join-Path $GitRoot $e.Path
        if (!(Test-Path -LiteralPath $p)) { continue }
        $bytes = [System.IO.File]::ReadAllBytes($p)
        $pe = [BitConverter]::ToInt32($bytes, 0x3C)
        $off = $pe + 0x18 + 70
        [BitConverter]::GetBytes([uint16]$e.OldDllChar).CopyTo($bytes, $off)
        [System.IO.File]::WriteAllBytes($p, $bytes)
    }
    Remove-Item -LiteralPath $manifestPath -Force
    Write-Host ("PE flags restored for {0} image(s)" -f $entries.Count)
} else {
    Write-Host "no PE flag manifest found; nothing to restore"
}

Write-Host "done."
