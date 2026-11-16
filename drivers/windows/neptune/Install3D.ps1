# SPDX-License-Identifier: MIT
param([switch]$ValidateOnly)
$ErrorActionPreference='Stop'
# PnPUtil 259 can mean no matching device OR an already-current device.
# Only the actual binding distinguishes these cases; never accept 259 blindly.
function Resolve-3DInstallResult {
    param([int]$Code,[object[]]$Bindings)
    $targets=@($Bindings | Where-Object { $_.DeviceID -like 'PCI\VEN_1AF4&DEV_1050*' })
    $healthy=@($targets | Where-Object {
        $_.Service -ieq 'VioGpu3D' -and $_.InfName -match '^oem[0-9]+\.inf$' -and
        $_.DriverVersion -and ([version]$_.DriverVersion -ge [version]'100.6.101.58000') -and
        $null -ne $_.ProblemCode -and $_.ProblemCode -eq 0
    })
    $state='failed';$success=$false;$restart=$false
    switch($Code) {
        0 { $state='command-completed';$success=$true }
        3010 { $state='restart-required';$success=$true;$restart=$true }
        1641 { $state='restart-initiated';$success=$true;$restart=$true }
        259 {
            if($targets.Count -gt 0 -and $healthy.Count -eq $targets.Count) {
                $state='already-current';$success=$true
            } else { $state='not-bound-to-healthy-3d-driver' }
        }
    }
    [pscustomobject]@{ExitCode=$Code;Succeeded=$success;State=$state;RestartRequired=$restart;
        TargetCount=$targets.Count;Healthy3DCount=$healthy.Count}
}
function Read-3DBindings {
    $devices=@{}
    foreach($device in @(Get-CimInstance Win32_PnPEntity -Filter "PNPClass='Display'")) {
        $devices[$device.DeviceID]=$device
    }
    foreach($driver in @(Get-CimInstance Win32_PnPSignedDriver | Where-Object DeviceID -Like 'PCI\VEN_1AF4&DEV_1050*')) {
        $device=$devices[$driver.DeviceID]
        [pscustomobject]@{DeviceName=$driver.DeviceName;DeviceID=$driver.DeviceID;InfName=$driver.InfName;
            DriverVersion=$driver.DriverVersion;Service=$(if($device){$device.Service}else{$null});
            ProblemCode=$(if($device){$device.ConfigManagerErrorCode}else{$null})}
    }
}
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
$old=@(Read-3DBindings)
$old | Format-List | Out-String | Set-Content $log -Encoding UTF8
Write-Host 'Installing the Microsoft-catalog-signed ARM64X 3D display package. The display may briefly change.'
& "$env:SystemRoot\System32\pnputil.exe" /add-driver (Join-Path $destination 'viogpu3d.inf') /install 2>&1 | Tee-Object -FilePath $log -Append
$code=$LASTEXITCODE
# Always record the post-command binding, including the already-current case.
$current=@(Read-3DBindings)
$current | Format-List | Out-String | Add-Content $log -Encoding UTF8
$outcome=Resolve-3DInstallResult -Code $code -Bindings $current
$outcome | ConvertTo-Json | Add-Content $log -Encoding UTF8
if(-not $outcome.Succeeded) { throw "Driver command returned $code ($($outcome.State)); see $log" }
if($outcome.State -eq 'already-current') {
    Write-Host 'The 3D driver is already bound to the device. No reinstall was needed.'
} else { Write-Host 'Driver installation command completed.' }
Write-Host 'Keep 3D enabled in the app. Driver binding alone does not confirm hardware 3D rendering.'
if($outcome.RestartRequired) { Write-Host 'Windows reports that a restart is required or already initiated.' }
Write-Host "No restart was requested by this script. Log: $log"
exit 0
