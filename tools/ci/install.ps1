#Requires -Version 5.1
<#
.SYNOPSIS
  Drop the patched msys-2.0.dll into a Git for Windows ("Git Bash") install.

.DESCRIPTION
  For each copy of the runtime (usr\bin\msys-2.0.dll and bin\msys-2.0.dll)
  the existing DLL is renamed to msys-2.0.dll.stock and the patched one is
  installed in its place, so uninstall.ps1 can restore the original.

  It also opts every PE image in the install into ASLR (the DYNAMICBASE
  flag).  Sandboxes built on the Windows Process Security Environment
  (PSEC) force-relocate images that did not opt in, which breaks Cygwin's
  same-address-space fork (children die with 0xC0000142).  Every changed
  flag is recorded in a manifest so uninstall.ps1 can revert it; use
  -SkipPeFlags to leave the images alone.

  Keep this script next to msys-2.0.dll (as they are laid out in the
  release).  Installing into "C:\Program Files\Git" needs an elevated
  (Administrator) prompt.

.PARAMETER GitRoot
  Root of the Git for Windows install.  Default: "C:\Program Files\Git".

.PARAMETER SkipPeFlags
  Only swap the DLL; do not change the PE flags of any image.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File install.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File install.ps1 -GitRoot 'D:\Git'
#>
[CmdletBinding()]
param(
    [string]$GitRoot = "C:\Program Files\Git",
    [switch]$SkipPeFlags
)

$ErrorActionPreference = "Stop"

# --- PE helpers ----------------------------------------------------------

function Get-PeInfo {
    param([string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x100) { return $null }
    if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { return $null }
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($pe -le 0 -or ($pe + 0x18 + 0x70 + 48) -gt $bytes.Length) { return $null }
    if ([BitConverter]::ToUInt32($bytes, $pe) -ne 0x00004550) { return $null }
    $opt = $pe + 0x18
    if ([BitConverter]::ToUInt16($bytes, $opt) -ne 0x20B) { return $null }  # PE32+ only
    $chars = [BitConverter]::ToUInt16($bytes, $pe + 0x16)
    $relocRva = [BitConverter]::ToUInt32($bytes, $opt + 0x70 + 5 * 8)
    [pscustomobject]@{
        Bytes        = $bytes
        DllCharOff   = $opt + 70
        DllChar      = [BitConverter]::ToUInt16($bytes, $opt + 70)
        HasRelocs    = (($chars -band 0x0001) -eq 0) -and ($relocRva -ne 0)
    }
}

function Set-PeDllChar {
    param([byte[]]$Bytes, [int]$Offset, [uint16]$Value)
    [BitConverter]::GetBytes($Value).CopyTo($Bytes, $Offset)
}

# --- sanity checks -------------------------------------------------------

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$dll = Join-Path $scriptDir "msys-2.0.dll"
if (!(Test-Path $dll)) {
    throw "msys-2.0.dll not found next to this script ($scriptDir). Keep the script and the DLL in the same directory."
}
if (!(Test-Path (Join-Path $GitRoot "usr\bin\bash.exe"))) {
    throw "Not a Git for Windows install: $GitRoot"
}

# --- 1. swap the runtime DLL (originals renamed to .stock) ---------------

foreach ($rel in @("usr\bin", "bin")) {
    $dir = Join-Path $GitRoot $rel
    if (!(Test-Path -LiteralPath $dir)) { continue }
    $target = Join-Path $dir "msys-2.0.dll"
    $backup = Join-Path $dir "msys-2.0.dll.stock"
    if (Test-Path -LiteralPath $target) {
        if (!(Test-Path -LiteralPath $backup)) {
            Move-Item -LiteralPath $target -Destination $backup
            Write-Host "backed up  : $rel\msys-2.0.dll -> msys-2.0.dll.stock"
        } else {
            Write-Host "backup kept: $rel\msys-2.0.dll.stock (pre-existing)"
            Remove-Item -LiteralPath $target -Force
        }
    }
    try {
        Copy-Item -LiteralPath $dll -Destination $target
    } catch {
        throw "Cannot write $target (run this script from an elevated prompt?): $($_.Exception.Message)"
    }
    Write-Host "installed  : $rel\msys-2.0.dll"
}

# --- 2. PE ASLR opt-in sweep (revertible via the manifest) ---------------

$manifestPath = Join-Path (Join-Path $GitRoot "usr\bin") "msys-2.0.dll.dropin.json"

if ($SkipPeFlags) {
    Write-Host "PE flag sweep skipped (-SkipPeFlags)."
} else {
    $changed = @()
    Get-ChildItem -LiteralPath $GitRoot -Recurse -Include @("*.exe", "*.dll") -File | ForEach-Object {
        $info = Get-PeInfo $_.FullName
        if ($null -eq $info) { return }
        if (($info.DllChar -band 0x0040) -ne 0) { return }   # already opted in
        if (!$info.HasRelocs) { return }                     # cannot ASLR anyway
        Set-PeDllChar -Bytes $info.Bytes -Offset $info.DllCharOff -Value ([uint16]($info.DllChar -bor 0x0040))
        [System.IO.File]::WriteAllBytes($_.FullName, $info.Bytes)
        $changed += [pscustomobject]@{
            Path       = $_.FullName.Substring($GitRoot.Length).TrimStart("\", "/")
            OldDllChar = $info.DllChar
        }
    }
    ($changed | ConvertTo-Json) | Set-Content -LiteralPath $manifestPath -Encoding ASCII
    Write-Host ("PE flag sweep: {0} image(s) opted into ASLR (manifest: {1})" -f $changed.Count, $manifestPath)
}

Write-Host "done. Run uninstall.ps1 to revert."
