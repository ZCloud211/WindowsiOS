# SPDX-License-Identifier: MIT
# Compile only. Never installs a driver, changes boot options or signing policy.
param([switch]$CheckOnly, [string]$OutputDirectory = "")
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $projectRoot 'build\windows-gpu' }
$msbuild = Get-Command msbuild.exe -ErrorAction SilentlyContinue
if (-not $msbuild) { throw 'Open a Visual Studio 2022 Developer PowerShell with the ARM64 C++ tools and WDK installed.' }
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$headers = @(Get-ChildItem (Join-Path $kits 'Include') -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName 'km\dispmprt.h') } |
    Sort-Object Name -Descending)
if (-not $headers.Count) { throw 'Windows WDK headers (km\dispmprt.h) are missing.' }
$kitVersion = $headers[0].Name
$armLibrary = Join-Path $kits "Lib\$kitVersion\km\arm64\displib.lib"
if (-not (Test-Path $armLibrary)) { throw 'The WDK ARM64 display-driver libraries are missing.' }
Write-Output "WDK $kitVersion and ARM64 display libraries found."
if ($CheckOnly) { exit 0 }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$driver = Join-Path $PSScriptRoot 'kmdod\Sample\SampleDisplay.vcxproj'
$probe = Join-Path $PSScriptRoot 'probe\WinGpuProbe.vcxproj'
$diagnostic = Join-Path $PSScriptRoot 'diagnostic\WinGpuDiagnostic.vcxproj'
foreach ($entry in @(@{Project=$driver;Name='driver'}, @{Project=$probe;Name='probe'}, @{Project=$diagnostic;Name='diagnostic'})) {
    $out = (Join-Path $OutputDirectory $entry.Name) + '\'
    $intermediate = (Join-Path $OutputDirectory ($entry.Name + '-obj')) + '\'
    $arguments = @($entry.Project, '/t:Build', '/m', '/p:Configuration=Release', '/p:Platform=ARM64', '/p:PreferredToolArchitecture=x64',
        "/p:WindowsTargetPlatformVersion=$kitVersion", '/p:SignMode=Off', "/p:OutDir=$out", "/p:IntDir=$intermediate")
    & $msbuild.Source @arguments
    if ($LASTEXITCODE -ne 0) { throw "$($entry.Name) build failed with exit code $LASTEXITCODE" }
}
Write-Output 'Built the experimental driver and probe. They have not been signed, installed or run.'
