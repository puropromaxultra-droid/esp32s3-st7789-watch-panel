@echo off
setlocal
if "%~1"=="" (
  echo.
  echo   把 GIF 文件直接拖到本文件图标上，就会自动转换。
  echo.
  pause
  exit /b
)
set PY=C:\Users\ZhuanZ\.workbuddy\binaries\python\versions\3.13.12\python.exe
"%PY%" "%~dp0gif2h.py" "%~1" -o "%~dp0..\lcd_fast\gif_data.h" --max-kb 850 --fit cover
copy /y "%~dp0..\lcd_fast\gif_data.h" "%~dp0..\lcd_player\gif_data.h" >nul
echo.
echo   [完成] 打开 Arduino IDE 里的 lcd_fast，重新编译烧录即可
echo   （输出尺寸/帧数见上面一行）
echo.
pause
