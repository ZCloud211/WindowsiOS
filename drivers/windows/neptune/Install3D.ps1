# SPDX-License-Identifier: MIT
param([switch]$ValidateOnly)
$ErrorActionPreference='Stop'
$files=@('viogpu3d.inf','viogpu3d.cat','viogpu3d.sys','neptune_umd.dll','neptune_umd_arm64.dll','neptune_umd_ec.dll','neptune_umd_x86.dll','vgpusrv.exe','viogpuap.exe','virtio_icd.json','vulkan_virtio.dll')
foreach($name in $files) { if(-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $name))) { throw "Missing driver file: $name" } }
$inf=Get-Content (Join-Path $PSScriptRoot 'viogpu3d.inf') -Raw
if($inf -notmatch '100\.6\.101\.58000' -or $inf -notmatch 'NTARM64') { throw 'Wrong ARM64 3D package.' }
if($ValidateOnly) { Write-Output 'PASS: complete pinned ARM64X 3D package; no driver installed.';exit 0 }
if($env:PROCESSOR_ARCHITECTURE -ne 'ARM64') { throw 'Windows ARM64 is required.' }
$principal=New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$PSCommandPath+'"'
    $process=Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    exit $process.ExitCode
}
$destination=Join-Path $env:SystemDrive 'WINIOS3D'
New-Item -ItemType Directory -Force $destination | Out-Null
foreach($name in ($files+@('viogpudo.inf','viogpudo.cat','viogpudo.sys','Rollback3D.ps1','Rollback3D.cmd'))) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $destination -Force
}
$log=Join-Path $destination 'INSTALL.LOG'
$old=Get-CimInstance Win32_PnPSignedDriver | Where-Object DeviceID -Like 'PCI\VEN_1AF4&DEV_1050*'
$old | Select-Object DeviceName,InfName,DriverVersion,DeviceID | Format-List | Out-String | Set-Content $log -Encoding UTF8
Write-Host 'Installing the Microsoft-catalog-signed ARM64X 3D display package. The display may briefly change.'
& "$env:SystemRoot\System32\pnputil.exe" /add-driver (Join-Path $destination 'viogpu3d.inf') /install 2>&1 | Tee-Object -FilePath $log -Append
$code=$LASTEXITCODE
if($code -notin @(0,3010)) { throw "Driver installation returned $code; see $log" }
Get-CimInstance Win32_PnPSignedDriver | Where-Object DeviceID -Like 'PCI\VEN_1AF4&DEV_1050*' |
    Select-Object DeviceName,InfName,DriverVersion,DeviceID | Format-List | Out-String | Add-Content $log -Encoding UTF8
Write-Host 'Driver installation command completed. Keep 3D enabled in the app when starting Windows.'
Write-Host "A restart may be required. No automatic restart was requested. Log: $log"
