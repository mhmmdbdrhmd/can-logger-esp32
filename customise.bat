@echo off
REM Double-click me, or drag a .dbc file onto me.
REM
REM Opens the logger's web page against your frame map so the dashboard and the
REM sendable values can be laid out at a desk, with no hardware connected.
setlocal
cd /d "%~dp0"
where py >nul 2>nul && (py -3 customise.py %* & goto :eof)
where python >nul 2>nul && (python customise.py %* & goto :eof)
echo Python is not installed, or is not on PATH.
echo Install it from https://www.python.org/downloads/ and tick
echo "Add python.exe to PATH" during setup.
pause
