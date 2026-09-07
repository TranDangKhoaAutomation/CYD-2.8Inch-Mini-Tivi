@echo off
setlocal EnableExtensions
cd /d "%~dp0"

where winget >nul 2>nul || (
  echo Khong tim thay winget. Hay cap nhat App Installer tu Microsoft Store, roi chay lai file nay.
  pause
  exit /b 1
)

call :install Python.Python.3.12 "Python 3.12"
if errorlevel 1 exit /b 1
call :install Gyan.FFmpeg.Shared FFmpeg
if errorlevel 1 exit /b 1
call :install OpenJS.NodeJS.LTS "Node.js LTS"
if errorlevel 1 exit /b 1

set "PYTHON="
if exist "%LocalAppData%\Programs\Python\Python312\python.exe" set "PYTHON=%LocalAppData%\Programs\Python\Python312\python.exe"
for /f "delims=" %%P in ('where python 2^>nul') do if not defined PYTHON set "PYTHON=%%P"
if not defined PYTHON if exist "%USERPROFILE%\.platformio\penv\Scripts\python.exe" set "PYTHON=%USERPROFILE%\.platformio\penv\Scripts\python.exe"
if not defined PYTHON (
  echo Python da duoc yeu cau cai dat nhung chua co trong PATH. Dong cua so nay, mo lai va chay install.bat mot lan nua.
  pause
  exit /b 1
)

"%PYTHON%" -m pip install --upgrade pip
if errorlevel 1 exit /b 1
"%PYTHON%" -m pip install --only-binary=:all: -r tools\youtube_tv_server\requirements.txt
if errorlevel 1 exit /b 1

echo.
echo Cai dat xong. Mo mot Command Prompt moi, sau do chay start_server.bat.
pause
exit /b 0

:install
echo Dang cai %~2...
winget install --id %~1 --exact --source winget --silent --accept-package-agreements --accept-source-agreements
exit /b %errorlevel%
