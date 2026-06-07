@echo off
rem Build the Tour uber .cja archive and run it (Windows).
rem The uber bundles LLVM-23 bitcode — including the compiler-synthesized C
rem `main` dispatcher (it marshals argv into the entry's String[]) — so we link
rem it with the cajeta-llvm fork's clang (a stock clang can't read LLVM-23
rem bitcode) and run. No external entry / shim is needed.
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..") do set "REPO_ROOT=%%~fI"
if not defined CAJETA_BIN set "CAJETA_BIN=%REPO_ROOT%\build\src\cajeta.exe"
if not defined CLANG_BIN  set "CLANG_BIN=%REPO_ROOT%\..\cajeta-llvm\build-cajeta\bin\clang-23.exe"

set "SRC_ROOT=%SCRIPT_DIR%src"
set "UBER_DIR=%SCRIPT_DIR%build\uber"
set "RUN_DIR=%SCRIPT_DIR%build\uber-run"
set "ENTRY=tour.Tour.main"

if not exist "%CAJETA_BIN%" (
    echo error: cajeta compiler not found at %CAJETA_BIN% 1>&2
    exit /b 1
)

rem 1. Build the uber archive.
if exist "%UBER_DIR%" rmdir /s /q "%UBER_DIR%"
mkdir "%UBER_DIR%"
echo Compiling cajeta sources -^> .cja archive ^(uber^)
echo   entry: %ENTRY%
"%CAJETA_BIN%" --emit=uber "%ENTRY%" "%SRC_ROOT%" "%UBER_DIR%" > "%UBER_DIR%\cajeta-compile.log" 2>&1
if errorlevel 1 (
    echo error: cajeta --emit=uber failed ^(see %UBER_DIR%\cajeta-compile.log^) 1>&2
    type "%UBER_DIR%\cajeta-compile.log"
    exit /b 1
)

set "CJA="
for /r "%UBER_DIR%" %%f in (*.cja) do if not defined CJA set "CJA=%%f"
if not defined CJA (
    echo error: no .cja produced by the uber build 1>&2
    exit /b 1
)

if not exist "%CLANG_BIN%" (
    echo error: LLVM-23 clang not found at %CLANG_BIN% 1>&2
    echo        set CLANG_BIN to the cajeta-llvm fork's clang 1>&2
    exit /b 1
)

rem 2. Extract the bundled bitcode.
if exist "%RUN_DIR%" rmdir /s /q "%RUN_DIR%"
mkdir "%RUN_DIR%"
echo [run] extracting bitcode from %CJA%
"%CAJETA_BIN%" archive extract "%CJA%" -C "%RUN_DIR%" > nul

set "BCS="
for /r "%RUN_DIR%" %%f in (*.bc) do set "BCS=!BCS! "%%f""
if not defined BCS (
    echo error: no bitcode entries in the archive 1>&2
    exit /b 1
)

rem 3. Link the bitcode into a native binary and run it.
set "OUT_BINARY=%RUN_DIR%\tour-uber.exe"
echo [run] linking bitcode -^> %OUT_BINARY%
"%CLANG_BIN%" -ffunction-sections -fdata-sections -Wl,--gc-sections -o "%OUT_BINARY%" !BCS! -lpthread -lm
if errorlevel 1 (
    echo error: link failed 1>&2
    exit /b 1
)

echo.
echo === running %OUT_BINARY% ^(from the uber archive^) ===
"%OUT_BINARY%" %*
exit /b %errorlevel%
