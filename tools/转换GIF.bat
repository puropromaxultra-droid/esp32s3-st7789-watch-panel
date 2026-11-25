@echo off
setlocal
if "%~1"=="" (
  echo.
  echo   把 .gif / .png / .jpg 拖到本文件图标上 —— 自动转成 240x240 并追加到 U5 专辑
  echo   之后在 GIF 页长按 A / B 切换内容
  echo.
  pause
  exit /b
)
set PY=C:\Users\ZhuanZ\.workbuddy\binaries\python\versions\3.13.12\python.exe
"%PY%" "%~dp0album.py" add "%~1"
echo.
pause
