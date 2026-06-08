@echo off
rem Build and run the tour with the cajeta build tool (reads cajeta.json).
setlocal
set "SCRIPT_DIR=%~dp0"
if not defined CAJETA set "CAJETA=%SCRIPT_DIR%..\..\build\src\cajeta.exe"
if not exist "%CAJETA%" (
    echo error: cajeta build tool not found at "%CAJETA%" 1>&2
    echo        build the compiler first, or set CAJETA=\path\to\cajeta.exe 1>&2
    exit /b 1
)
cd /d "%SCRIPT_DIR%"
"%CAJETA%" run %*
