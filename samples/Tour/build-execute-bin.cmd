@echo off
rem Build the Tour to a native binary and run it (Windows).
rem Mirrors build-execute-bin.sh: cajeta --emit=obj per source, then link the
rem .o files with clang into tour.exe, then run.
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..") do set "REPO_ROOT=%%~fI"
if not defined CAJETA_BIN set "CAJETA_BIN=%REPO_ROOT%\build\src\cajeta.exe"
if not defined CLANG_BIN  set "CLANG_BIN=clang"

set "SRC_ROOT=%SCRIPT_DIR%src"
set "BUILD_DIR=%SCRIPT_DIR%build\bin"
set "OUT_BINARY=%SCRIPT_DIR%build\tour.exe"
set "ENTRY=tour.Tour.main"

if not exist "%CAJETA_BIN%" (
    echo error: cajeta compiler not found at %CAJETA_BIN% 1>&2
    echo        build the compiler first: cd %REPO_ROOT% ^&^& build.bat 1>&2
    exit /b 1
)

if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"

echo [1/2] Compiling cajeta sources -^> object files
echo       entry: %ENTRY%
"%CAJETA_BIN%" --emit=obj "%ENTRY%" "%SRC_ROOT%" "%BUILD_DIR%" > "%BUILD_DIR%\cajeta-compile.log" 2>&1
if errorlevel 1 (
    echo error: cajeta --emit=obj failed ^(see %BUILD_DIR%\cajeta-compile.log^) 1>&2
    type "%BUILD_DIR%\cajeta-compile.log"
    exit /b 1
)

set "OBJS="
for /r "%BUILD_DIR%" %%f in (*.o) do set "OBJS=!OBJS! "%%f""
if not defined OBJS (
    echo error: cajeta produced no .o files ^(see %BUILD_DIR%\cajeta-compile.log^) 1>&2
    exit /b 1
)

echo [2/2] Linking -^> %OUT_BINARY%
"%CLANG_BIN%" -o "%OUT_BINARY%" !OBJS! -lpthread -lm -Wl,--gc-sections
if errorlevel 1 (
    echo error: link failed 1>&2
    exit /b 1
)

echo.
echo === running %OUT_BINARY% ===
"%OUT_BINARY%" %*
exit /b %errorlevel%
