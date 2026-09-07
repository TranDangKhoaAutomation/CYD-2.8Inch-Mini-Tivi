@echo off
setlocal
cd /d %~dp0
set "PYTHON="
if exist "%LocalAppData%\Programs\Python\Python312\python.exe" set "PYTHON=%LocalAppData%\Programs\Python\Python312\python.exe"
for /f "delims=" %%P in ('where python 2^>nul') do if not defined PYTHON set "PYTHON=%%P"
if not defined PYTHON if exist "%USERPROFILE%\.platformio\penv\Scripts\python.exe" set "PYTHON=%USERPROFILE%\.platformio\penv\Scripts\python.exe"
if not defined PYTHON (echo Python khong co trong PATH. Cai Python 3.9+ hoac PlatformIO. & pause & exit /b 1)
if exist "%ProgramFiles%\nodejs" set "PATH=%ProgramFiles%\nodejs;%PATH%"
if not defined FFMPEG_DIR if exist "%LocalAppData%\Microsoft\WinGet\Packages" for /r "%LocalAppData%\Microsoft\WinGet\Packages" %%F in (ffmpeg.exe) do if not defined FFMPEG_DIR set "FFMPEG_DIR=%%~dpF"
if defined FFMPEG_DIR set "PATH=%FFMPEG_DIR%;%PATH%"
where ffmpeg >nul 2>nul || (echo Khong tim thay ffmpeg trong PATH. Cai ffmpeg va them vao PATH. & pause & exit /b 1)
"%PYTHON%" -m pip install --only-binary=:all: -r requirements.txt
if errorlevel 1 pause & exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start_server_background.ps1" -Python "%PYTHON%" -ServerDir "%CD%"
exit /b %errorlevel%
