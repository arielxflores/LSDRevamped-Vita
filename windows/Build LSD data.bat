@echo off
rem LSD Revamped Vita - builds the game data (the lsdr folder for ux0:data/) on Windows.
rem Everything it needs is in this folder; it downloads LSD Revamped from GitHub (about 450 MB).
setlocal
cd /d "%~dp0"
set "PATH=%~dp0python;%~dp0ffmpeg;%PATH%"
set "PYTHONUTF8=1"
set "LSDR_CJK_FONT=%~dp0font\NotoSansCJK-Regular.ttc"
echo LSD Revamped Vita - game data builder
echo.
echo This downloads LSD Revamped from GitHub and converts it for the Vita.
echo It needs about 1.5 GB of free space and takes 10 to 30 minutes.
echo.
"%~dp0python\python.exe" "%~dp0tools\build_data.py" --lsdr "%~dp0work\LSDRevamped" --out "%~dp0work"
if errorlevel 1 (
  echo.
  echo Something went wrong: see the messages above. Running this again resumes where it stopped.
  pause
  exit /b 1
)
if exist "%~dp0lsdr" rmdir /s /q "%~dp0lsdr"
move "%~dp0work\lsdr" "%~dp0lsdr" >nul
echo.
echo Done. Copy the "lsdr" folder in this folder to ux0:data/ on your Vita
echo (so that you have ux0:data/lsdr/dreams.pak), then start the game.
echo The "work" folder can be deleted.
explorer "%~dp0"
pause
