<#
.SYNOPSIS
  Install AshDesk from the unpacked ZIP. Run from an elevated PowerShell.
.DESCRIPTION
  Copies the package to -Destination, adds the "AshDesk" Windows Firewall rules,
  creates the Start-menu shortcut, registers the uninstaller and downloads
  Cisco's OpenH264 DLL. Drivers and the service are installed only on request:
  -Drivers vdisplay,vaudio and -Service. Running the script again (also from the
  install directory) adds a driver or the service later. Copying a package over an
  existing install first stops the service and every program running from its bin\,
  and starts the service again afterwards.
  -InPlace installs from where the package is, without copying it and without touching
  the Start-menu shortcut or the uninstaller registration: the desktop UI runs it like
  this, so a portable copy (an unpacked ZIP) gets a service or a driver but no Start-menu
  entry and no "Installed apps" entry.
  -DryRun (alias -WhatIf) prints every action without changing anything.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\install.ps1 -DryRun
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\install.ps1 -Drivers vdisplay -Service -ServiceHostArgs "--psk-file C:\ProgramData\AshDesk\psk.txt --inject"
#>
#Requires -RunAsAdministrator
#Requires -PSEdition Desktop
[CmdletBinding()]
param(
    [string]$Destination = (Join-Path $env:ProgramFiles 'AshDesk'),
    # Overrides -Destination with this script's directory (see the description).
    [switch]$InPlace,
    # Development drivers are test-signed: the .cer goes into LocalMachine Root and TrustedPublisher.
    [string]$TestCertificate,
    # Drivers are installed on demand, none by default: vdisplay (adrd_host --capture idd) and/or
    # vaudio (remote microphone). "powershell -File" passes "a,b" as one string, so it is split below.
    [string[]]$Drivers = @(),
    # Install adrd_svc: LocalSystem, automatic start. It starts adrd_host with -ServiceHostArgs in the
    # console session (before logon, on the lock screen and on UAC prompts) and restarts it when it exits.
    [switch]$Service,
    [string]$ServiceHostArgs = '--inject --control-indicator',
    # Let the service press Ctrl+Alt+Del for the client (sets the SoftwareSASGeneration policy).
    [switch]$AllowSas,
    # Let the service own the virtual display, e.g. 1920x1080@60: plugged when the service starts and
    # kept while the host restarts or the console changes session. Needs -Drivers vdisplay.
    [string]$ServiceVirtualDisplay,
    # Automatic updates: a daily scheduled task (SYSTEM) runs "ashdesk_update apply --manifest URL".
    # The updater only takes packages signed with the key built into it and waits while a session is active.
    [string]$UpdateManifest,
    [switch]$SkipOpenH264,
    # Only Invoke-Main gets -WhatIf: under "powershell -File" the script scope is the global scope,
    # where a WhatIfPreference would also leak into every module imported on the way.
    [Alias('WhatIf')][switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$FirewallGroup = 'AshDesk'
$ServiceName = 'AshDeskSvc'    # default service name shared with the engine (see docs)
$UpdateTask = 'AshDesk Update'
# Install-OpenH264 and its pinned URL / hashes; get-openh264.ps1 (portable package) uses the same file
. (Join-Path $PSScriptRoot 'openh264.ps1')

function Invoke-Main {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param()
    $package = $PSScriptRoot
    if ($InPlace) { $Destination = $package }
    $bin = Join-Path $Destination 'bin'
    $wanted = Get-DriverNames $Drivers
    $resume = Copy-Package $package $Destination
    if ($wanted -and $TestCertificate) { Import-TestCertificate $TestCertificate }
    if ($wanted -contains 'vdisplay') {
        Install-AshDriver (Join-Path $package 'drivers\vdisplay\AshVD.inf') (Join-Path $package 'bin\adrd_vd.exe')
    }
    if ($wanted -contains 'vaudio') {
        $vaudio = Join-Path $package 'drivers\vaudio\AshVA.inf'
        if ((Test-Path $vaudio) -and (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control').SystemStartOptions -notmatch 'TESTSIGNING') {
            throw 'Virtual audio not installed: its kernel driver is test-signed and test signing is off (proto\vaudio\README.AshDesk.md).'
        }
        Install-AshDriver $vaudio (Join-Path $package 'bin\adrd_va.exe')
    }
    Add-AshFirewallRules $bin $FirewallGroup
    if (-not $InPlace) {
        if (Test-Path (Join-Path $package 'bin\ashdesk.exe')) {
            New-AshShortcut (Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'AshDesk.lnk') (Join-Path $bin 'ashdesk.exe')
        }
        Register-AshUninstall $Destination
    }
    if (-not $SkipOpenH264) { Install-OpenH264 $bin }
    if ($Service) { Install-AshService $bin $ServiceHostArgs $AllowSas $ServiceVirtualDisplay }
    elseif ($resume) { Invoke-Native sc.exe @('start', $ServiceName) }
    if ($UpdateManifest) { Register-AshUpdateTask $bin $UpdateManifest }
    if (-not $WhatIfPreference) {
        Write-Host "AshDesk installed in $Destination; remove it with $Destination\uninstall.ps1"
        # The build each installed program reports (commit, "-dirty" if built from changed sources): a stale exe shows up here
        foreach ($exe in 'adrd_host', 'adrd_client', 'adrd_svc') {
            $path = Join-Path $bin "$exe.exe"
            if (Test-Path $path) { Write-Host ('  ' + ((& $path --version) -join ' ')) }
        }
        if (-not $wanted) { Write-Host 'No driver installed. Add one when a feature needs it: install.ps1 -Drivers vdisplay (or vaudio).' }
    }
}

function Get-DriverNames([string[]]$Given) {
    $names = @($Given | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ })
    foreach ($name in $names) {
        if ('vdisplay', 'vaudio' -notcontains $name) { throw "-Drivers: unknown driver '$name' (vdisplay, vaudio)" }
    }
    $names
}

function Invoke-Native {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$FilePath, [string[]]$Arguments)
    if ($PSCmdlet.ShouldProcess("$FilePath $($Arguments -join ' ')", 'Run')) {
        & $FilePath @Arguments
        if ($LASTEXITCODE -ne 0) { throw "$FilePath $($Arguments -join ' ') exited with $LASTEXITCODE" }
    }
}

# Returns whether a running service had to be stopped for the copy, so the caller can start it again.
function Copy-Package {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$From, [string]$To)
    if ([IO.Path]::GetFullPath($From).TrimEnd('\') -ieq [IO.Path]::GetFullPath($To).TrimEnd('\')) { return $false }
    $resume = Stop-InstalledCopy (Join-Path $To 'bin')
    New-Item -ItemType Directory -Force $To | Out-Null
    Copy-Item -Path (Join-Path $From '*') -Destination $To -Recurse -Force
    $resume
}

# Files in use cannot be replaced, and a failed Copy-Item leaves the install half old, half new: stop the service
# running from this bin\ (its host goes with it) and every other program started from there before copying.
function Stop-InstalledCopy {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$Bin)
    $prefix = $Bin.TrimEnd('\') + '\'
    $service = Get-CimInstance Win32_Service -Filter "Name='$ServiceName'" -ErrorAction SilentlyContinue
    $running = $service -and $service.State -eq 'Running' -and $service.PathName.Trim('"').StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
    if ($running) { Stop-Service $ServiceName -Force }
    foreach ($process in Get-Process) {
        $path = $process.Path
        if ($path -and $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            if (-not $WhatIfPreference) { Write-Host "Stopping $($process.ProcessName) (pid $($process.Id)) to replace its files" }
            Stop-Process -Id $process.Id -Force
        }
    }
    $running
}

function Import-TestCertificate {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$Certificate)
    foreach ($store in 'Root', 'TrustedPublisher') {
        if ($PSCmdlet.ShouldProcess("Cert:\LocalMachine\$store", "Import $Certificate")) {
            Import-Certificate -FilePath $Certificate -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
        }
    }
}

function Install-AshDriver {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$Inf, [string]$Tool)
    # Drivers are only installed on request, so a request that cannot be met is an error: the caller
    # (the desktop app asks "install now?") must not be told that the driver is there.
    if (-not (Test-Path $Inf)) { throw "Driver not installed: $Inf is not in this package." }
    # pnputil only stages the package, which checks its signature; the tool then creates the root
    # device (pnputil cannot) and binds the driver, so a rejected signature leaves no driverless device.
    Invoke-Native pnputil.exe @('/add-driver', $Inf)
    Invoke-Native $Tool @('install', $Inf)
}

function Add-AshFirewallRules {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$Bin, [string]$Group)
    # NetSecurity cmdlets are CDXML module functions: -WhatIf given to this function does not reach
    # them, so every firewall change goes through this function's own ShouldProcess.
    foreach ($rule in Get-NetFirewallRule -Group $Group -ErrorAction SilentlyContinue) {
        if ($PSCmdlet.ShouldProcess($rule.DisplayName, 'Remove firewall rule')) { $rule | Remove-NetFirewallRule }
    }
    foreach ($exe in 'adrd_host', 'adrd_client') {
        $name = "$Group $exe (UDP in)"
        $program = Join-Path $Bin "$exe.exe"
        if ($PSCmdlet.ShouldProcess($name, "Add firewall rule: allow inbound UDP to $program")) {
            New-NetFirewallRule -DisplayName $name -Group $Group -Direction Inbound -Action Allow -Protocol UDP -Program $program | Out-Null
        }
    }
}

function New-AshShortcut {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$Link, [string]$Target)
    if ($PSCmdlet.ShouldProcess($Link, "Create shortcut to $Target")) {
        $shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut($Link)
        $shortcut.TargetPath = $Target
        $shortcut.WorkingDirectory = Split-Path $Target
        $shortcut.Save()
    }
}

function Register-AshUninstall {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$Dir)
    # "Installed apps" entry: without it the only way to find the uninstaller is to know the directory.
    $key = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\AshDesk'
    if (-not $PSCmdlet.ShouldProcess($key, 'Register uninstaller')) { return }
    $host_exe = Join-Path $Dir 'bin\adrd_host.exe'
    New-Item -Path $key -Force | Out-Null
    Set-ItemProperty $key DisplayName 'AshDesk'
    Set-ItemProperty $key Publisher 'AshDesk'
    Set-ItemProperty $key InstallLocation $Dir
    Set-ItemProperty $key DisplayIcon $host_exe
    Set-ItemProperty $key UninstallString "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$Dir\uninstall.ps1`""
    Set-ItemProperty $key NoModify 1 -Type DWord
    Set-ItemProperty $key NoRepair 1 -Type DWord
    if (Test-Path $host_exe) { Set-ItemProperty $key DisplayVersion (Get-Item $host_exe).VersionInfo.ProductVersion }
}

function Install-AshService {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$Bin, [string]$HostArgs, [bool]$Sas, [string]$VirtualDisplay)
    $svc = Join-Path $Bin 'adrd_svc.exe'
    if (-not $WhatIfPreference -and -not (Test-Path $svc)) {
        Write-Warning "Service skipped: $svc is not in this package."
        return
    }
    # A second run replaces the service, so its host arguments can be changed by installing again.
    if (Get-Service $ServiceName -ErrorAction SilentlyContinue) { Invoke-Native $svc @('uninstall') }
    # Windows PowerShell 5.1 does not escape quotes inside native arguments: adrd_svc gets the host
    # arguments as one argument only if the quotes inside are backslash-escaped here.
    $arguments = @('install', '--host-exe', (Join-Path $Bin 'adrd_host.exe'), '--host-args', ($HostArgs -replace '"', '\"'))
    if ($Sas) { $arguments += '--allow-sas' }
    if ($VirtualDisplay) { $arguments += @('--vd', $VirtualDisplay) }
    Invoke-Native $svc $arguments
    Invoke-Native sc.exe @('start', $ServiceName)
}

function Register-AshUpdateTask {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$Bin, [string]$Manifest)
    # ScheduledTasks cmdlets are CDXML functions like the firewall ones: they ignore an outer -WhatIf.
    if (-not $PSCmdlet.ShouldProcess($UpdateTask, "Register daily task: ashdesk_update apply --manifest $Manifest")) { return }
    $action = New-ScheduledTaskAction -Execute (Join-Path $Bin 'ashdesk_update.exe') -Argument "apply --manifest `"$Manifest`""
    # A random delay keeps a fleet of machines from asking the update server in the same minute.
    $trigger = New-ScheduledTaskTrigger -Daily -At '03:00' -RandomDelay (New-TimeSpan -Hours 2)
    $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -RunLevel Highest
    Register-ScheduledTask -TaskName $UpdateTask -Action $action -Trigger $trigger -Principal $principal -Force | Out-Null
}

# Dot-sourcing (". .\install.ps1") only defines the functions, for testing single steps.
if ($MyInvocation.InvocationName -ne '.') { Invoke-Main -WhatIf:$DryRun }
