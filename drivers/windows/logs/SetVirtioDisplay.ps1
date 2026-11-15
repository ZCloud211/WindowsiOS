# SPDX-License-Identifier: MIT
# User-launched, one-shot display configuration; 30-second automatic rollback.
param([switch]$SelfTest, [switch]$InspectOnly)
$ErrorActionPreference='Stop'
$source=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'DisplayTopology.cs'),[Text.Encoding]::UTF8)
Add-Type -TypeDefinition $source -Language CSharp
[WinIOSDisplayTopology]::SelfTest()
if ($SelfTest) { Write-Output 'PASS: Win32 display ABI, PCI identifier selection, missing/ambiguous adapter refusal; no display changed.'; exit 0 }
$before=[WinIOSDisplayTopology]::Read()
if ($InspectOnly) { $before | ConvertTo-Json -Depth 6; exit 0 }
$drive=New-Object IO.DriveInfo([IO.Path]::GetPathRoot($PSScriptRoot))
if ($drive.VolumeLabel -ne 'WINIOSDRV') { throw 'Run this tool from WINIOSDRV.' }
$target=[WinIOSDisplayTopology]::Select($before)
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$folder=Join-Path (Join-Path $PSScriptRoot 'DISPLAYLOGS') ((Get-Date).ToString('yyyyMMdd_HHmmss')+'_'+[Guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Force $folder | Out-Null
$encoding=New-Object Text.UTF8Encoding($true)
function Save-File([string]$Name,[string]$Text) {
    [byte[]]$bytes=$encoding.GetPreamble()+$encoding.GetBytes($Text)
    $stream=[IO.File]::Create((Join-Path $folder $Name))
    try { $stream.Write($bytes,0,$bytes.Length);$stream.Flush($true) } finally { $stream.Dispose() }
}
function Save-Display([string]$Name,$Data) { Save-File $Name ($Data | ConvertTo-Json -Depth 8) }
function Flush-Logs { try { Write-VolumeCache -DriveLetter $drive.Name.Substring(0,1) -ErrorAction Stop } catch {} }
Save-Display 'BEFORE.JSON' $before
$result=[ordered]@{format_version=1;started_utc=[DateTime]::UtcNow.ToString('o');target_id=$target.id;target_name=$target.name;applied=$false;confirmed=$false;reverted=$false;completed=$false;errors=@()}
Save-Display 'RESULT.JSON' $result
Flush-Logs
$attempted=$false
try {
    Write-Host '正在将 PCI VirtIO GPU 设置为唯一主屏…'
    $attempted=$true
    [WinIOSDisplayTopology]::ApplyVirtioOnly($before)
    $result.applied=$true
    Save-Display 'APPLIED.JSON' ([WinIOSDisplayTopology]::Read())
    # The dialog is centered on the explicitly selected primary display. A
    # black or unreadable result times out and restores the complete old layout.
    $form=New-Object Windows.Forms.Form
    $form.Text='确认 VirtIO 显示'
    $form.Size=New-Object Drawing.Size(460,190)
    $form.StartPosition='CenterScreen';$form.TopMost=$true
    $form.FormBorderStyle='FixedDialog';$form.MaximizeBox=$false;$form.MinimizeBox=$false
    $label=New-Object Windows.Forms.Label
    $label.Text='桌面、窗口和光标是否完整可见？'+[Environment]::NewLine+'选择 保留；30 秒未确认将自动恢复原布局。'
    $label.Location=New-Object Drawing.Point(18,20);$label.Size=New-Object Drawing.Size(420,55)
    $keep=New-Object Windows.Forms.Button
    $keep.Text='保留(&Y)';$keep.DialogResult=[Windows.Forms.DialogResult]::OK
    $keep.Location=New-Object Drawing.Point(215,90);$keep.Size=New-Object Drawing.Size(95,32)
    $revert=New-Object Windows.Forms.Button
    $revert.Text='恢复原布局';$revert.DialogResult=[Windows.Forms.DialogResult]::Cancel
    $revert.Location=New-Object Drawing.Point(320,90);$revert.Size=New-Object Drawing.Size(110,32)
    $form.Controls.AddRange(@($label,$keep,$revert));$form.AcceptButton=$keep;$form.CancelButton=$revert
    $timer=New-Object Windows.Forms.Timer;$timer.Interval=30000
    $timer.Add_Tick({$form.DialogResult=[Windows.Forms.DialogResult]::Cancel;$form.Close()})
    $form.Add_Shown({$form.Activate();$keep.Focus();$timer.Start()})
    try { $answer=$form.ShowDialog() } finally { $timer.Stop();$timer.Dispose();$form.Dispose() }
    if ($answer -eq [Windows.Forms.DialogResult]::OK) { $result.confirmed=$true }
    else { [WinIOSDisplayTopology]::Restore($before);$result.reverted=$true }
    $result.completed=$true
} catch {
    $result.errors += $_.Exception.Message
    if ($attempted) {
        try { [WinIOSDisplayTopology]::Restore($before);$result.reverted=$true }
        catch { $result.errors += 'Restore failed: '+$_.Exception.Message }
    }
} finally {
    try { Save-Display 'AFTER.JSON' ([WinIOSDisplayTopology]::Read()) } catch { $result.errors += $_.Exception.Message }
    $result.finished_utc=[DateTime]::UtcNow.ToString('o')
    Save-Display 'RESULT.JSON' $result
    Save-File 'DONE.TXT' 'Display setup finished.'
    Flush-Logs
}
if ($result.confirmed -and $result.completed) { Write-Host '已保存 VirtIO 唯一主屏设置。请回复“显示已修复”，我会直接读取结果。';exit 0 }
if ($result.reverted) { Write-Host '已恢复原布局。请回复“显示已恢复原布局”，我会读取记录。';exit 2 }
Write-Host '显示设置未完成，请保留窗口并告诉我。'
exit 1
