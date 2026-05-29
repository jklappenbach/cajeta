@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ===========================================================================
rem Run the RELEASE regression subset on native Windows -- analogue of
rem release_tests.sh. Runs only the cross-compilation-sensitive suites listed
rem in test\release_filter.txt, not the full battery (run_tests.cmd). See that
rem file for the suite list + rationale.
rem
rem Usage:
rem   release_tests.cmd                  ::  build (incremental) then run subset
rem   set NO_BUILD=1 ^&^& release_tests.cmd  ::  run against an already-built tree
rem   release_tests.cmd --gtest_brief=0  ::  extra flags pass through to binary
rem
rem Knobs (set before running, e.g. `set NO_BUILD=1`):
rem   NO_BUILD=1   skip the incremental build step.
rem   MSYS2_ROOT   MSYS2 install root (default C:\msys64); its mingw64\bin is
rem                prepended to PATH so cmake/ninja AND the test binary's
rem                runtime DLLs (libstdc++, antlr4-runtime, ...) resolve.
rem
rem Exit status is the test binary's, so this works directly as a release gate.
rem ===========================================================================

rem Work from the repo root (this script lives there).
cd /d "%~dp0"
set "ROOT=%CD%"

if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
if exist "%MSYS2_ROOT%\mingw64\bin" set "PATH=%MSYS2_ROOT%\mingw64\bin;%PATH%"

set "TEST_BIN=build\test\cajeta_test.exe"
set "FILTER_FILE=test\release_filter.txt"

if not exist "%FILTER_FILE%" ( echo error: %FILTER_FILE% not found & exit /b 1 )

rem --- Configure / build ------------------------------------------------------
if not exist "build\build.ninja" (
    echo ^>^> No build\ found, running setup.cmd
    call setup.cmd || ( echo error: setup.cmd failed & exit /b 1 )
)
if not defined NO_BUILD (
    echo ^>^> cmake --build build
    cmake --build build -j %NUMBER_OF_PROCESSORS% || ( echo error: build failed & exit /b 1 )
)
if not exist "%TEST_BIN%" ( echo error: %TEST_BIN% not built & exit /b 1 )

rem The test binary reads its .cajeta fixtures relative to this root.
set "CAJETA_SOURCE_ROOT=%ROOT%"

rem --- Parse the filter file into a single `:`-joined gtest filter ------------
rem Skip blank lines and `#` comments. `eol=#` makes for /f drop whole-line
rem comments; we additionally skip lines whose first token starts with `#`.
set "filter="
set "npat=0"
for /f "usebackq eol=# tokens=* delims=" %%L in ("%FILTER_FILE%") do (
    set "line=%%L"
    rem Trim leading spaces/tabs.
    for /f "tokens=* delims= " %%T in ("!line!") do set "line=%%T"
    if defined line (
        if defined filter ( set "filter=!filter!:!line!" ) else ( set "filter=!line!" )
        set /a npat+=1
    )
)

if %npat%==0 ( echo error: %FILTER_FILE% lists no patterns & exit /b 1 )

rem --- Drift guard: fail if the filter matches zero tests --------------------
rem gtest treats a zero-match filter as success (0 run); a renamed/removed
rem suite would silently drop coverage. Count matches before the real run.
set "nmatch=0"
for /f %%a in ('"%TEST_BIN%" --gtest_filter=!filter! --gtest_list_tests 2^>nul ^| findstr /R "^  [a-zA-Z]" ^| find /c /v ""') do set "nmatch=%%a"
if %nmatch%==0 (
    echo error: release filter matched zero tests -- a suite was renamed or
    echo        removed. Update %FILTER_FILE%.
    exit /b 1
)

echo ^>^> Release subset: %npat% suites, %nmatch% tests
"%TEST_BIN%" --gtest_filter=!filter! --gtest_brief=1 %*
exit /b %errorlevel%
