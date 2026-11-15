# SPDX-License-Identifier: MIT
# Run on the ephemeral build runner only. Never exports the private signing key.
param([string]$BuildDirectory = 'build/windows-gpu')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Write-Output 'Preparing diagnostic files.'
$built = Get-ChildItem "$BuildDirectory/diagnostic" -Recurse -Filter wingpudiag.inf | Select-Object -First 1
if (-not $built) { throw 'Diagnostic INF missing.' }
$package = Join-Path $BuildDirectory 'package'
New-Item -ItemType Directory -Force $package | Out-Null
Copy-Item $built.FullName "$package/wingpudiag.inf"
Copy-Item "$($built.DirectoryName)/WinGpuDiagnostic.sys" $package
Copy-Item "$BuildDirectory/probe/WinGpuProbe.exe" $package
Copy-Item 'gpu/LICENSE' "$package/LICENSE.txt"
Copy-Item 'drivers/windows/diagnostic/TESTING.md' "$package/README.md"
Write-Output 'Creating disposable non-exportable test signing key.'
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=WindowsiOS GPU diagnostic test only' `
    -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 2048 `
    -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddMonths(3)
Export-Certificate -Cert $cert -FilePath "$package/WinGpuDiagnostic-test.cer" | Out-Null
$tool = Get-ChildItem drivers/windows/packages -Recurse -Filter signtool.exe |
    Where-Object FullName -Match '\\x64\\' | Select-Object -First 1
if (-not $tool) { throw 'Missing x64 signtool.' }
Write-Output 'Signing diagnostic sys.'
& $tool.FullName sign /v /fd SHA256 /s My /sha1 $cert.Thumbprint "$package/WinGpuDiagnostic.sys"
if ($LASTEXITCODE) { throw 'Embedded test signature failed.' }
# Rebuild catalog AFTER embedding the sys signature; signing changes its hash.
Write-Output 'Generating Windows 11 ARM64 catalog.'
& inf2cat.exe "/driver:$((Resolve-Path $package).Path)" /os:10_GE_ARM64 /uselocaltime
if ($LASTEXITCODE) { throw 'Diagnostic catalog generation failed.' }
Write-Output 'Signing diagnostic catalog.'
& $tool.FullName sign /v /fd SHA256 /s My /sha1 $cert.Thumbprint "$package/wingpudiagnostic.cat"
if ($LASTEXITCODE) { throw 'Catalog test signature failed.' }
# Trust is confined to this disposable runner. No guest boot policy is changed.
Write-Output 'Trusting test certificate in disposable runner machine stores (no current-user root consent UI).'
& certutil.exe -f -addstore Root "$package/WinGpuDiagnostic-test.cer"
if ($LASTEXITCODE) { throw 'Runner test root import failed.' }
& certutil.exe -f -addstore TrustedPublisher "$package/WinGpuDiagnostic-test.cer"
if ($LASTEXITCODE) { throw 'Runner test publisher import failed.' }
Write-Output 'Verifying embedded signature and catalog membership.'
& $tool.FullName verify /pa /v "$package/WinGpuDiagnostic.sys"
if ($LASTEXITCODE) { throw 'Embedded signature verification failed.' }
& $tool.FullName verify /pa /v /c "$package/wingpudiagnostic.cat" "$package/WinGpuDiagnostic.sys"
if ($LASTEXITCODE) { throw 'Catalog signature or driver membership verification failed.' }
@{
    source_commit=$env:GITHUB_SHA; build_run=$env:GITHUB_RUN_ID
    architecture='ARM64'; certificate_thumbprint=$cert.Thumbprint
    test_signed=$true; production_signed=$false; windows_load_verified=$false
    direct3d_implemented=$false; private_key_exported=$false
} | ConvertTo-Json | Set-Content "$package/BUILD.json" -Encoding utf8
Get-ChildItem $package -File | Where-Object Name -NE 'SHA256SUMS.txt' | Sort-Object Name | ForEach-Object {
    "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())  $($_.Name)"
} | Set-Content "$package/SHA256SUMS.txt" -Encoding ascii
Write-Output "Packaged ARM64 diagnostics with test certificate $($cert.Thumbprint). Windows loading is not verified."
