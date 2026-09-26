@echo off
setlocal
rem Drop your Melee NTSC 1.02 ISO onto this file. Builds the port first if it has not been built yet.
cd /d "%~dp0"
set ISO=%~1
if "%ISO%"=="" set ISO=%~dp0melee.iso
if not exist "%ISO%" (
  echo Drop your Melee NTSC 1.02 ISO onto play.bat, or put it next to it named melee.iso
  pause
  exit /b 1
)
where python >nul 2>nul && (
  rem A compressed disc (.ciso, what Dolphin and many backups use) becomes a plain melee.iso here, once.
  for %%I in ("%ISO%") do if /i "%%~xI"==".ciso" (
    echo Converting the .ciso to a plain ISO ^(once, about 1.4 GB^)...
    python tools\ciso_to_iso.py "%ISO%" "%~dp0melee.iso" || goto :ciso_fail
    set ISO=%~dp0melee.iso
  )
)
if not exist build-review\port\Release\melee_port.exe (
  echo No build yet; building from source first.
  call build.bat "%ISO%" || exit /b 1
)
build-review\port\Release\melee_port.exe --iso "%ISO%" --threaded-renderer --fps unlocked --frame-mode authored --scale auto --volume 70
if errorlevel 1 pause
exit /b 0

:ciso_fail
echo Could not convert the .ciso. In Dolphin: right-click the game, Convert File, format ISO, then drop that .iso here.
pause
exit /b 1
