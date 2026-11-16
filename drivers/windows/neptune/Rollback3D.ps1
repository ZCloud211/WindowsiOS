# SPDX-License-Identifier: MIT
$ErrorActionPreference='Stop'
$principal=New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$PSCommandPath+'"'
    $process=Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    exit $process.ExitCode
}
$drivers=@(Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceID -like 'PCI\VEN_1AF4&DEV_1050*' })
foreach($driver in $drivers) {
    if($driver.InfName -notmatch '^oem\d+\.inf$') { continue }
    $installed=Join-Path "$env:SystemRoot\INF" $driver.InfName
    if((Get-Content $installed -Raw) -notmatch 'viogpu3d\.sys') { continue }
    & "$env:SystemRoot\System32\pnputil.exe" /delete-driver $driver.InfName /uninstall /force
    if($LASTEXITCODE -notin @(0,3010)) { throw 'Could not remove the 3D driver.' }
}
& "$env:SystemRoot\System32\pnputil.exe" /add-driver (Join-Path $PSScriptRoot 'viogpudo.inf') /install
if($LASTEXITCODE -notin @(0,3010)) { throw 'Could not restore the original display driver.' }
Write-Host 'Original 2D display driver restored. No automatic restart was requested.'
