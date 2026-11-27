<#
.SYNOPSIS
  Remove AshDesk installed by install.ps1. Run from an elevated PowerShell.
.DESCRIPTION
  Reverse order of install.ps1: stops and removes the service, stops the AshDesk
  processes (virtual displays disappear about 1 s after adrd_host stops pinging
  the driver), removes the virtual display and virtual audio devices and every
  driver-store package of theirs, the "AshDesk" firewall rules, the Start-menu
  shortcut, the uninstaller registration and the install directory.
  -DryRun (alias -WhatIf) prints every action without changing anything.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File "$env:ProgramFiles\AshDesk\uninstall.ps1" -DryRun
#>
#Requires -RunAsAdministrator
# Get-WindowsDriver fails with "Class not registered" under PowerShell 7.
#Requires -PSEdition Desktop
[CmdletBinding()]
param(
    # Defaults to this script's directory (resolved below: $PSScriptRoot is empty in param defaults on PowerShell 5.1).
    [string]$InstallDir,
    # Only Invoke-Main gets -WhatIf: under "powershell -File" the script scope is the global scope,
    # where a WhatIfPreference would also leak into the Dism module imported by Get-WindowsDriver.
    [Alias('WhatIf')][switch]$DryRun
)

$ErrorActionPreference = 'Stop'
if (-not $InstallDir) { $InstallDir = $PSScriptRoot }
$FirewallGroup = 'AshDesk'
$ServiceName = 'AshDeskSvc'    # default service name shared with the engine (see docs)
$UpdateTask = 'AshDesk Update'
$DriverInfs ='ashvd.inf', 'ashva.inf'

function Invoke-Main {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param()
    $bin = Join-Path $InstallDir 'bin'
    # The update task first, then the service: neither may start anything while the rest is removed.
    # ScheduledTasks cmdlets are CDXML functions and ignore -WhatIf, hence the explicit ShouldProcess.
    if ((Get-ScheduledTask -TaskName $UpdateTask -ErrorAction SilentlyContinue) -and $PSCmdlet.ShouldProcess($UpdateTask, 'Remove scheduled task')) {
        Unregister-ScheduledTask -TaskName $UpdateTask -Confirm:$false
    }
    # The service would restart the host that Stop-AshProcesses is about to stop.
    if (Get-Service $ServiceName -ErrorAction SilentlyContinue) { Invoke-Native (Join-Path $bin 'adrd_svc.exe') @('uninstall') }
    if (Stop-AshProcesses $bin) {
        # The driver watchdog unplugs the virtual displays about 1 s after the host stops (AVD_WATCHDOG_MS).
        if (-not $WhatIfPreference) { Start-Sleep -Seconds 2 }
    }
    foreach ($tool in 'adrd_vd.exe', 'adrd_va.exe') {
        if (Test-Path (Join-Path $bin $tool)) { Invoke-Native (Join-Path $bin $tool) @('uninstall') }
    }
    Remove-DriverPackages $DriverInfs
    Remove-AshFirewallRules $FirewallGroup
    $shortcut = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'AshDesk.lnk'
    if (Test-Path $shortcut) { Remove-Item $shortcut }
    $registration = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\AshDesk'
    if (Test-Path $registration) { Remove-Item $registration -Recurse }
    Remove-InstallDir $InstallDir
}

function Invoke-Native {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$FilePath, [string[]]$Arguments)
    if ($PSCmdlet.ShouldProcess("$FilePath $($Arguments -join ' ')", 'Run')) {
        & $FilePath @Arguments
        if ($LASTEXITCODE -ne 0) { throw "$FilePath $($Arguments -join ' ') exited with $LASTEXITCODE" }
    }
}

function Stop-AshProcesses {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$Bin)
    $prefix = $Bin.TrimEnd('\') + '\'
    $stopped = $false
    foreach ($process in Get-Process) {
        $path = $process.Path
        if ($path -and $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            Stop-Process -Id $process.Id -Force
            $stopped = $true
        }
    }
    $stopped
}

function Remove-DriverPackages {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string[]]$Names)
    # Every driver build gets a new DriverVer, so older packages stay in the driver store next to the
    # one adrd_vd / adrd_va just removed; match them by original INF name, not by published oemNN.inf.
    foreach ($driver in Get-WindowsDriver -Online) {
        if ($Names -contains (Split-Path $driver.OriginalFileName -Leaf)) {
            Invoke-Native pnputil.exe @('/delete-driver', $driver.Driver, '/uninstall', '/force')
        }
    }
}

function Remove-AshFirewallRules {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$Group)
    # NetSecurity cmdlets are CDXML module functions and ignore -WhatIf given to this function.
    foreach ($rule in Get-NetFirewallRule -Group $Group -ErrorAction SilentlyContinue) {
        if ($PSCmdlet.ShouldProcess($rule.DisplayName, 'Remove firewall rule')) { $rule | Remove-NetFirewallRule }
    }
}

function Remove-InstallDir {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$Dir)
    if (-not (Test-Path (Join-Path $Dir 'bin\adrd_host.exe'))) {
        Write-Warning "Not removing $Dir`: it does not look like an AshDesk install (no bin\adrd_host.exe)."
        return
    }
    Set-Location $env:SystemRoot
    Remove-Item $Dir -Recurse -Force
}

# Dot-sourcing (". .\uninstall.ps1") only defines the functions, for testing single steps.
if ($MyInvocation.InvocationName -ne '.') { Invoke-Main -WhatIf:$DryRun }
