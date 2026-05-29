@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ===========================================================================
rem Run the RELEASE regression subset on native Windows -- analogue of
rem release_tests.sh. Runs only the cross-compilation-sensitive suites listed
rem in test\release_filter.txt, not the full battery (run_tests.cmd with no
rem args). See that file for the suite list + rationale.
rem
rem The sharded execution + crash reporting is reused from run_tests.cmd (this
rem script just selects the subset and delegates).
rem
rem Usage:
rem   release_tests.cmd                  ::  build (incremental) then run subset
rem   set NO_BUILD=1 ^&^& release_tests.cmd  ::  run against an already-built tree
rem
rem Knobs (set before running, e.g. `set NO_BUILD=1`):
rem   NO_BUILD=1   skip the incremental build step.
rem   PARALLEL=N   shard count (default: CPU count, capped at 32). PARALLEL=0
rem                forces a serial run.
rem   MSYS2_ROOT   MSYS2 install root (default C:\msys64); its mingw64\bin is
rem                prepended to PATH so cmake/ninja AND the test binary's
rem                runtime DLLs resolve.
rem
rem Exit status is run_tests.cmd's, so this works directly as a release gate.
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

rem --- Parse the filter file into a space-separated pattern list -------------
rem Skip blank lines and `#` comments (eol=# drops whole-line comments).
set "pats="
set "filter="
set "npat=0"
for /f "usebackq eol=# tokens=* delims=" %%L in ("%FILTER_FILE%") do (
    set "line=%%L"
    for /f "tokens=* delims= " %%T in ("!line!") do set "line=%%T"
    if defined line (
        set "pats=!pats! !line!"
        if defined filter ( set "filter=!filter!:!line!" ) else ( set "filter=!line!" )
        set /a npat+=1
    )
)

if %npat%==0 ( echo error: %FILTER_FILE% lists no patterns & exit /b 1 )

rem --- Drift guard: fail if the filter matches zero tests --------------------
rem gtest treats a zero-match filter as success (0 run); total wipeout (every
rem suite renamed) would silently pass. Count matches before the real run.
rem (release_tests.sh additionally pinpoints partial drift per-pattern; that's
rem the authoritative guard run in CI.)
set "CAJETA_SOURCE_ROOT=%ROOT%"
set "nmatch=0"
for /f %%a in ('"%TEST_BIN%" --gtest_filter=!filter! --gtest_list_tests 2^>nul ^| findstr /R "^  [a-zA-Z]" ^| find /c /v ""') do set "nmatch=%%a"
if %nmatch%==0 (
    echo error: release filter matched zero tests -- a suite was renamed or
    echo        removed. Update %FILTER_FILE%.
    exit /b 1
)

rem --- Delegate to run_tests.cmd for sharded execution -----------------------
rem PARALLEL forced (default CPU count) so the subset runs in parallel even
rem though it's a filtered run; run_tests.cmd's parallel discovery honors
rem these patterns. NO_BUILD avoids a redundant second build.
if not defined PARALLEL set "PARALLEL=%NUMBER_OF_PROCESSORS%"
echo ^>^> Release subset: %npat% suites, %nmatch% tests (delegating to run_tests.cmd, parallel)
set "NO_BUILD=1"
call "%ROOT%\run_tests.cmd"%pats%
exit /b %errorlevel%
