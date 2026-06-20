@echo off
rem Build the profile benchmark harness with the cajeta build tool.
rem Reads cajeta.json and runs its `build` task -> build\profile.
rem Override the compiler with the CAJETA environment variable.
setlocal
set "SCRIPT_DIR=%~dp0"
set "REPO_ROOT=%SCRIPT_DIR%..\.."
if "%CAJETA%"=="" set "CAJETA=%REPO_ROOT%\build-cajeta\src\cajeta"
if not exist "%CAJETA%" (
    echo error: cajeta build tool not found at %CAJETA% 1>&2
    echo        build the compiler first, or set CAJETA=\path\to\cajeta 1>&2
    exit /b 1
)
cd /d "%SCRIPT_DIR%"
"%CAJETA%" build %*
