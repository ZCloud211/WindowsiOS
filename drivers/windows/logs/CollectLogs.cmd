@echo off
chcp 65001 >nul
echo 正在收集 Windows 蓝屏日志。若出现权限提示，请选择“是”。
if not exist "%~dp0CRASHLOGS" mkdir "%~dp0CRASHLOGS"
echo Started %date% %time%>"%~dp0CRASHLOGS\LAUNCH.TXT"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0CollectCrash.ps1"
if errorlevel 1 (
  echo 日志工具未完成，请保留此窗口。
) else (
  echo 已完成。告诉我“已收集”即可，不需要复制或截图。
)
pause
