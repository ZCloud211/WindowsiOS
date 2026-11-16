@echo off
chcp 65001 >nul
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install3D.ps1"
if errorlevel 1 echo Installation did not complete. Keep this window and collect the log.
pause
