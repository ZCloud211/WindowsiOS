# SPDX-License-Identifier: MIT
# Run the production classifier without running the installer, CIM or PnPUtil.
$ErrorActionPreference='Stop'
$tokens=$null;$parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Install3D.ps1'),[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count) { throw ($parseErrors | Out-String) }
$function=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Resolve-3DInstallResult'},$true)
if(-not $function) { throw 'Missing production return-code classifier.' }
. ([scriptblock]::Create($function.Extent.Text))
$healthy=[pscustomobject]@{DeviceID='PCI\VEN_1AF4&DEV_1050&SUBSYS_11001AF4';Service='VioGpu3D';InfName='oem20.inf';DriverVersion='100.6.101.58000';ProblemCode=0}
$old=[pscustomobject]@{DeviceID=$healthy.DeviceID;Service='viogpudo';InfName='oem19.inf';DriverVersion='100.103.104.30200';ProblemCode=0}
$broken=[pscustomobject]@{DeviceID=$healthy.DeviceID;Service='VioGpu3D';InfName='oem20.inf';DriverVersion=$healthy.DriverVersion;ProblemCode=43}
$unknown=[pscustomobject]@{DeviceID=$healthy.DeviceID;Service='VioGpu3D';InfName='oem20.inf';DriverVersion=$healthy.DriverVersion;ProblemCode=$null}
$cases=@(
    @{Code=259;Bindings=@($healthy);Success=$true;State='already-current';Restart=$false},
    @{Code=259;Bindings=@();Success=$false;State='not-bound-to-healthy-3d-driver';Restart=$false},
    @{Code=259;Bindings=@($old);Success=$false;State='not-bound-to-healthy-3d-driver';Restart=$false},
    @{Code=259;Bindings=@($broken);Success=$false;State='not-bound-to-healthy-3d-driver';Restart=$false},
    @{Code=259;Bindings=@($unknown);Success=$false;State='not-bound-to-healthy-3d-driver';Restart=$false},
    @{Code=259;Bindings=@($healthy,$old);Success=$false;State='not-bound-to-healthy-3d-driver';Restart=$false},
    @{Code=0;Bindings=@($healthy);Success=$true;State='command-completed';Restart=$false},
    @{Code=3010;Bindings=@($healthy);Success=$true;State='restart-required';Restart=$true},
    @{Code=1641;Bindings=@($healthy);Success=$true;State='restart-initiated';Restart=$true},
    @{Code=5;Bindings=@($healthy);Success=$false;State='failed';Restart=$false}
)
foreach($case in $cases) {
    $actual=Resolve-3DInstallResult -Code $case.Code -Bindings $case.Bindings
    if($actual.Succeeded -ne $case.Success -or $actual.State -ne $case.State -or $actual.RestartRequired -ne $case.Restart) {
        throw ('Incorrect return-code handling: '+($case | ConvertTo-Json -Compress)+' => '+($actual | ConvertTo-Json -Compress))
    }
}
Write-Output "PASS: $($cases.Count) installation outcomes; already-current succeeds only with healthy Neptune device bindings. No device or system changes."
