@echo off
rem Regenerate the Cajeta API reference (cajetadoc) from the runtime stdlib
rem source into site\docs\cajetadocs.
rem
rem Usage:
rem   scripts\regen-cajetadocs.cmd [-b]
rem
rem   -b   Force a rebuild of the cajetadoc tool before generating.
rem        (By default the tool is built only if its binary is missing.)
rem
rem Overridable via environment:
rem   OUT_DIR  output directory      (default: site\docs\cajetadocs)
rem   SRC_ROOT source root to scan   (default: runtime\src)
rem   TITLE    header project title  (default: Cajeta)
rem   VERSION  header version        (default: contents of .\VERSION)
rem   DATE     header publish date   (default: today, YYYY-MM-DD)
rem   LICENSE  header license type   (default: Apache-2.0)

setlocal enabledelayedexpansion

rem Repo root = parent of this script's directory.
set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%.." || exit /b 1
set "ROOT=%CD%"

set "FORCE_BUILD=0"
if /i "%~1"=="-b"      set "FORCE_BUILD=1"
if /i "%~1"=="--build" set "FORCE_BUILD=1"

if not defined OUT_DIR  set "OUT_DIR=site\docs\cajetadocs"
if not defined SRC_ROOT set "SRC_ROOT=runtime\src"
if not defined TITLE    set "TITLE=Cajeta"
if not defined LICENSE  set "LICENSE=Apache-2.0"

if not defined VERSION (
    set "VERSION=0.0.0"
    if exist "VERSION" set /p VERSION=<VERSION
)

if not defined DATE (
    for /f %%d in ('powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd"') do set "DATE=%%d"
)

set "BIN=build\tools\cajetadoc\cajetadoc.exe"

if not exist "build\" (
    echo error: no build\ directory. Configure the project first ^(cmake -S . -B build^). 1>&2
    popd & exit /b 1
)

if "%FORCE_BUILD%"=="1" goto build
if not exist "%BIN%"     goto build
goto generate

:build
echo ^>^> building cajetadoc tool
cmake --build build --target cajetadoc
if errorlevel 1 ( popd & exit /b 1 )

:generate
if not exist "%BIN%" (
    echo error: cajetadoc binary not found at %BIN% after build. 1>&2
    popd & exit /b 1
)

echo ^>^> generating docs
echo    source : %SRC_ROOT%
echo    output : %OUT_DIR%
echo    header : %TITLE% v%VERSION%  %DATE%  %LICENSE%

"%BIN%" "%SRC_ROOT%" -o "%OUT_DIR%" --project-title "%TITLE%" --project-version "%VERSION%" --date-published "%DATE%" --project-license "%LICENSE%"
if errorlevel 1 ( popd & exit /b 1 )

echo ^>^> done. Open %OUT_DIR%\index.html
popd
endlocal
