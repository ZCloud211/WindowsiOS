# SPDX-License-Identifier: MIT
# Stage the unmodified official driver while the GPU is OFF; enable small crash
# records. No device installation, driver removal, signing-policy change or reboot.
param([switch]$ValidateOnly)
$ErrorActionPreference = 'Stop'
function Check-Package {
    $expected = @('SOURCE.json','viogpudo.cat','viogpudo.inf','viogpudo.sys')
    $seen = @{}
    foreach ($line in Get-Content (Join-Path $PSScriptRoot 'SHA256SUMS.txt') -Encoding UTF8) {
        if ($line -notmatch '^([0-9a-fA-F]{64})  ([A-Za-z0-9_.]+)$') { throw 'Invalid package manifest.' }
        $name=$Matches[2]; $digest=$Matches[1]
        if ($name -notin $expected -or $seen.ContainsKey($name)) { throw 'Unexpected package entry.' }
        if ((Get-FileHash (Join-Path $PSScriptRoot $name) -Algorithm SHA256).Hash -ne $digest) { throw "Package hash mismatch: $name" }
        $seen[$name]=$true
    }
    if ($seen.Count -ne 4) { throw 'Incomplete official driver package.' }
    $inf = Get-Content (Join-Path $PSScriptRoot 'viogpudo.inf') -Raw
    if ($inf -notmatch '100\.103\.104\.30200' -or $inf -notmatch 'NTarm64') { throw 'Incorrect driver version or architecture.' }
}
Check-Package
if ($ValidateOnly) { Write-Output 'PASS: official package hashes, ARM64 INF and version; no Windows configuration changed.'; exit 0 }
if ($env:PROCESSOR_ARCHITECTURE -ne 'ARM64') { throw 'This tool requires Windows ARM64.' }
$principal=New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$PSCommandPath+'"'
    $process=Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    exit $process.ExitCode
}
$drive=New-Object IO.DriveInfo([IO.Path]::GetPathRoot($PSScriptRoot))
if ($drive.VolumeLabel -ne 'WINIOSDRV') { throw 'Run this tool from WINIOSDRV.' }
# Refuse to proceed if the GPU is already present: staging must happen in the
# healthy RAMFB session, before the next boot can automatically load a driver.
$present=@(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })
if ($present.Count) { throw '请先正常关机，在 App 关闭 VirtIO 显示设备，再启动 Windows 运行此工具。' }
$destination=Join-Path $PSScriptRoot 'PREPLOGS'
New-Item -ItemType Directory -Force $destination | Out-Null
$result=[ordered]@{format_version=1;started_utc=[DateTime]::UtcNow.ToString('o');completed=$false;driver_staged=$false;crash_recording_enabled=$false;errors=@()}
$encoding=New-Object Text.UTF8Encoding($true)
function Save-Prep([string]$Name,[string]$Text) { [IO.File]::WriteAllText((Join-Path $destination $Name),$Text,$encoding) }
try {
    Write-Host '正在保存原设置并开启小型蓝屏记录…'
    $backup=Join-Path $destination 'CrashControl-before.reg'
    if (-not (Test-Path -LiteralPath $backup)) {
        & reg.exe export 'HKLM\SYSTEM\CurrentControlSet\Control\CrashControl' $backup /y | Out-Null
        if ($LASTEXITCODE) { throw 'Could not save original crash settings.' }
    }
    $key='HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl'
    New-ItemProperty $key -Name CrashDumpEnabled -Value 3 -PropertyType DWord -Force | Out-Null
    New-ItemProperty $key -Name LogEvent -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty $key -Name AutoReboot -Value 0 -PropertyType DWord -Force | Out-Null
    $settings=Get-ItemProperty $key
    if ($settings.CrashDumpEnabled -ne 3 -or $settings.LogEvent -ne 1 -or $settings.AutoReboot -ne 0) { throw 'Crash settings verification failed.' }
    $result.crash_recording_enabled=$true
    $paging=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management'
    $usage=@(Get-CimInstance Win32_PageFileUsage | Select-Object Name,AllocatedBaseSize,CurrentUsage)
    Save-Prep 'PAGEFILE.JSON' (@{configured=$paging.PagingFiles;active=$usage} | ConvertTo-Json -Depth 5)
    # Deliberately do not create/resize a paging file or allocate a large dump.
    Write-Host '正在将官方 302 版 ARM64 显示驱动加入 Windows 驱动库…'
    $output=(& pnputil.exe /add-driver (Join-Path $PSScriptRoot 'viogpudo.inf') 2>&1 | Out-String -Width 240)
    $code=$LASTEXITCODE
    Save-Prep 'STAGE.TXT' $output
    $result.driver_stage_exit=$code
    if ($code -ne 0 -and $code -ne 3010) { throw "Driver staging failed: $code" }
    $result.driver_staged=$true
    $output=(& pnputil.exe /enum-drivers 2>&1 | Out-String -Width 240)
    Save-Prep 'DRIVERS-AFTER.TXT' $output
    $result.completed=$true
} catch { $result.errors += $_.Exception.Message; Write-Host $_.Exception.Message }
finally {
    $result.finished_utc=[DateTime]::UtcNow.ToString('o')
    Save-Prep 'PREP.JSON' ($result | ConvertTo-Json -Depth 5)
    try { Write-VolumeCache -DriveLetter $drive.Name.Substring(0,1) -ErrorAction Stop } catch { Save-Prep 'FLUSH.TXT' $_.Exception.Message }
}
if (-not $result.completed) { exit 1 }
Write-Host '准备完成。请先回复“已准备驱动”，我核对日志后再进行下一次启动。'
Write-Host '无需截图或复制文字；本工具不会自动重启。'
exit 0
