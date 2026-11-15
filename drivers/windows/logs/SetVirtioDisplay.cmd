@echo off
chcp 65001 >nul
echo 正在识别 VirtIO 显卡。切换后画面正常请选择“保留”；否则 30 秒后自动恢复。
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0SetVirtioDisplay.ps1"
if errorlevel 2 (
  echo 已恢复原布局，请告诉我。
) else if errorlevel 1 (
  echo 设置未完成，请保留此窗口。
) else (
  echo 已完成。回复“显示已修复”即可，不需要截图。
)
pause
