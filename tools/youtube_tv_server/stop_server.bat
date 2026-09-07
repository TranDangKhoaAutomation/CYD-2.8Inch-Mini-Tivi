@echo off
setlocal
set "SERVER_DIR=%~dp0."
set "PID_FILE=%SERVER_DIR%\server.pid"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0stop_server_background.ps1" -ServerDir "%SERVER_DIR%"
endlocal
