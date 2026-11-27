<#
.SYNOPSIS
  Download Cisco's OpenH264 into this AshDesk folder's bin\ (software decoding without an NVIDIA GPU).
.DESCRIPTION
  For the portable package (unzipped, no install.ps1) and for installs done with install.ps1 -SkipOpenH264.
  Needs no administrator rights unless bin\ is somewhere only administrators may write (Program Files).
  Double-click get-openh264.cmd next to it, or run: powershell -ExecutionPolicy Bypass -File get-openh264.ps1
  Exit code 0 = the DLL is in bin\ and its sha256 matches; 1 = it is not.
#>
[CmdletBinding()]
param(
    # Defaults to bin\ next to this script (resolved below: $PSScriptRoot is empty in param defaults on PowerShell 5.1).
    [string]$Bin
)

$ErrorActionPreference = 'Stop'
if (-not $Bin) { $Bin = Join-Path $PSScriptRoot 'bin' }
. (Join-Path $PSScriptRoot 'openh264.ps1')

if (-not (Test-Path (Join-Path $Bin 'adrd_client.exe'))) {
    Write-Host -ForegroundColor Red "$Bin does not look like an AshDesk bin folder (no adrd_client.exe)."
    exit 1
}
$dll = Join-Path $Bin $OpenH264Dll
if ((Test-Path $dll) -and (Get-FileHash -Algorithm SHA256 $dll).Hash -eq $OpenH264DllSha256) {
    Write-Host "$OpenH264Dll is already in $Bin."
    exit 0
}
try {
    Install-OpenH264 $Bin
} catch [UnauthorizedAccessException] {
    Write-Host -ForegroundColor Red "No permission to write to $Bin. Right-click get-openh264.cmd and choose 'Run as administrator'."
    exit 1
} catch {
    Write-Host -ForegroundColor Red "Could not get $OpenH264Dll`: $($_.Exception.Message)"
    exit 1
}
if (-not (Test-Path $dll)) { exit 1 }    # tar without bzip2: Install-OpenH264 printed the manual steps
Write-Host "Done: $dll"
exit 0
