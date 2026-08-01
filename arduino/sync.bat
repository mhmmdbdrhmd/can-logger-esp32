@echo off
REM ===========================================================================
REM  Windows version of sync.sh - mirrors src\ into the Arduino IDE sketch
REM  folder. Just double-click it.
REM
REM  There is exactly one copy of the firmware, in src\. PlatformIO compiles it
REM  directly; the Arduino IDE needs the files next to a .ino named after the
REM  folder, so this makes that copy. Run it once after cloning, and again
REM  whenever you edit anything in src\.
REM ===========================================================================
setlocal

set "HERE=%~dp0"
set "SRC=%HERE%..\src"
set "DST=%HERE%CanLogger"

if not exist "%SRC%" (
    echo ERROR: cannot find "%SRC%"
    echo Keep this script inside the arduino\ folder next to src\.
    pause
    exit /b 1
)

if not exist "%DST%" mkdir "%DST%"

REM Remove previously synced sources first, so a file deleted in src\ also
REM disappears here instead of lingering and being compiled into the sketch.
del /q "%DST%\*.h" 2>nul
del /q "%DST%\*.cpp" 2>nul

REM main.cpp becomes the .ino (a deliberately tiny wrapper - see app.h);
REM everything else keeps its name.
copy /y "%SRC%\main.cpp" "%DST%\CanLogger.ino" >nul
for %%F in ("%SRC%\*.h" "%SRC%\*.cpp") do (
    if /i not "%%~nxF"=="main.cpp" copy /y "%%F" "%DST%\%%~nxF" >nul
)

echo.
echo Synced src\ to %DST%
echo Now open %DST%\CanLogger.ino in the Arduino IDE.
echo.
pause
