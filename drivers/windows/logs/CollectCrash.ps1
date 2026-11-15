# SPDX-License-Identifier: MIT
# Read Windows diagnostics; write only beside this tool on the removable image.
# No driver installation, registry writes, restart, or signing-policy changes.
param([string]$OutputRoot = '', [switch]$NoElevation, [switch]$NoFlush, [switch]$NoDumps)
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$admin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin -and -not $NoElevation) {
    Write-Host '请在权限提示中选择“是”，用于读取崩溃转储。'
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '"'
    if ($OutputRoot) { $arguments += ' -OutputRoot "' + $OutputRoot + '"' }
    $process = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    exit $process.ExitCode
}
if (-not $OutputRoot) {
    $drive = New-Object IO.DriveInfo([IO.Path]::GetPathRoot($PSScriptRoot))
    if ($drive.VolumeLabel -ne 'WINIOSDRV') { throw '请从 WINIOSDRV 工具盘运行此文件。' }
    $OutputRoot = Join-Path $PSScriptRoot 'CRASHLOGS'
}
$run = (Get-Date).ToString('yyyyMMdd_HHmmss') + '_' + ([Guid]::NewGuid().ToString('N').Substring(0,6))
$destination = Join-Path $OutputRoot $run
New-Item -ItemType Directory -Force $destination | Out-Null
$problems = New-Object 'System.Collections.Generic.List[string]'
$result = [ordered]@{
    format_version=1; started_utc=[DateTime]::UtcNow.ToString('o'); finished_utc=$null; completed=$false
    elevated=$admin; small_dumps=@(); dump_headers=@(); errors=@()
}
function Save-Text([string]$Name, [string]$Text) {
    $encoding = New-Object Text.UTF8Encoding($true)
    [IO.File]::WriteAllText((Join-Path $destination $Name),$Text,$encoding)
}
function Read-Command([string]$Name, [string]$Program, [string[]]$Arguments) {
    try {
        $text = (& $Program @Arguments 2>&1 | Out-String -Width 240)
        $code = $LASTEXITCODE
        Save-Text $Name $text
        if ($code) { $problems.Add("${Name}: native exit $code") }
    } catch { $problems.Add("${Name}: $($_.Exception.Message)"); Save-Text $Name ($_ | Out-String) }
}
Write-Host '正在读取蓝屏事件记录…'
$query = '*[System[(EventID=1001 or EventID=41 or EventID=6008 or EventID=161 or EventID=46)]]'
Read-Command 'EVENTS.XML' 'wevtutil.exe' @('qe','System',"/q:$query",'/rd:true','/c:32','/f:xml')
Read-Command 'EVENTS.TXT' 'wevtutil.exe' @('qe','System',"/q:$query",'/rd:true','/c:32','/f:text')
Write-Host '正在读取显示设备及转储设置…'
Read-Command 'DISPLAY.TXT' 'pnputil.exe' @('/enum-devices','/class','Display','/drivers')
Read-Command 'DRIVERS.TXT' 'pnputil.exe' @('/enum-drivers')
Read-Command 'CRASHCFG.TXT' 'reg.exe' @('query','HKLM\SYSTEM\CurrentControlSet\Control\CrashControl')
try {
    $os = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    $system = [ordered]@{
        product=$os.ProductName; build=$os.CurrentBuildNumber; ubr=$os.UBR
        architecture=$env:PROCESSOR_ARCHITECTURE; powershell=$PSVersionTable.PSVersion.ToString()
    }
    Save-Text 'SYSTEM.JSON' ($system | ConvertTo-Json)
} catch { $problems.Add("System metadata: $($_.Exception.Message)") }
if (-not $NoDumps) {
    Write-Host '正在收集已有的小型转储（不复制完整内存转储）…'
    try {
        $files = @(Get-ChildItem (Join-Path $env:SystemRoot 'Minidump') -Filter '*.dmp' -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 3)
        foreach ($file in $files) {
            if ($file.Length -eq 0 -or $file.Length -gt 8MB) {
                $problems.Add("Skipped oversized/empty minidump: $($file.Name), $($file.Length) bytes"); continue
            }
            $inputStream = $null; $outputStream = $null
            try {
                $inputStream = [IO.File]::Open($file.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
                $outputStream = [IO.File]::Create((Join-Path $destination $file.Name))
                $buffer = New-Object byte[] 65536
                $total = 0
                while (($count=$inputStream.Read($buffer,0,$buffer.Length)) -gt 0) {
                    $total += $count
                    if ($total -gt 8MB) { throw 'Minidump exceeded the copy limit.' }
                    $outputStream.Write($buffer,0,$count)
                }
                $outputStream.Flush($true)
                $result.small_dumps += $file.Name
            } catch { $problems.Add("Minidump $($file.Name): $($_.Exception.Message)") }
            finally { if ($outputStream) {$outputStream.Dispose()}; if ($inputStream) {$inputStream.Dispose()} }
        }
    } catch { $problems.Add("Minidump listing: $($_.Exception.Message)") }
    try {
        $settings = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl'
        $paths = @((Join-Path $env:SystemRoot 'MEMORY.DMP'))
        if ($settings.DumpFile) { $paths += [Environment]::ExpandEnvironmentVariables([string]$settings.DumpFile) }
        $index=0
        foreach ($path in ($paths | Select-Object -Unique)) {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
            $stream=$null
            try {
                $stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
                $header=New-Object byte[] 65536
                $count=$stream.Read($header,0,$header.Length)
                if ($count -le 0) {continue}
                $name="MEMORY$index.HEAD"
                $saved=New-Object byte[] $count;[Array]::Copy($header,$saved,$count)
                [IO.File]::WriteAllBytes((Join-Path $destination $name),$saved)
                $result.dump_headers += $name;$index++
            } catch { $problems.Add("Dump header: $($_.Exception.Message)") }
            finally { if ($stream) {$stream.Dispose()} }
        }
    } catch { $problems.Add("Dump header discovery: $($_.Exception.Message)") }
}
$result.completed=$true
$result.finished_utc=[DateTime]::UtcNow.ToString('o')
$result.errors=@($problems.ToArray())
Save-Text 'RESULT.JSON' ($result | ConvertTo-Json -Depth 5)
Save-Text 'DONE.TXT' 'Collection finished. Files are on WINIOSDRV; no screenshot or copy/paste needed.'
if (-not $NoFlush) {
    try {
        $root=[IO.Path]::GetPathRoot($destination)
        if ($root -match '^[A-Za-z]:\\$') { Write-VolumeCache -DriveLetter $root.Substring(0,1) -ErrorAction Stop }
    } catch { Save-Text 'FLUSH.TXT' ($_.Exception.Message) }
}
Write-Host '日志收集完成。直接告诉我“已收集”，无需复制输出或截图。'
exit 0
