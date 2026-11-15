@echo off
chcp 65001 >nul
echo 请保持 VirtIO 显示设备关闭，从 WINIOSDRV 运行本工具。
echo 将加入官方显示驱动并开启小型蓝屏记录，权限提示请选择“是”。
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0PrepareDisplay.ps1"
if errorlevel 1 (
  echo 准备未完成，请保留此窗口。
) else (
  echo 已完成。回复“已准备驱动”即可，不需要截图。
)
pause
