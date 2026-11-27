@echo off
setlocal
rem Double-click: download Cisco's OpenH264 into bin\ next to this file (see get-openh264.ps1)
rem Started from a PowerShell 7 prompt, PSModulePath points at PowerShell 7 modules and Windows PowerShell
rem then cannot find Get-FileHash: let it rebuild its own default
set "PSModulePath="
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0get-openh264.ps1" %*
set rc=%errorlevel%
pause
exit /b %rc%
