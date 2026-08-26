@echo off
setlocal
cd /d %~dp0
where python >nul 2>nul || (echo Python khong co trong PATH & pause & exit /b 1)
where ffmpeg >nul 2>nul || (echo Khong tim thay ffmpeg trong PATH. Cai ffmpeg va them vao PATH. & pause & exit /b 1)
python -m pip install -r requirements.txt
if errorlevel 1 pause & exit /b 1
python server.py
