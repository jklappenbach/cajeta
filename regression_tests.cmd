@echo off
setlocal enabledelayedexpansion
REM Crash-isolated parallel regression sweep (Windows native).
REM
REM Runs each gtest suite in cajeta_test in its OWN process, so a segfault/abort
REM in one suite can't truncate or bogus-tally the rest. Suites are launched
REM concurrently (throttled to a worker cap) and aggregated into a
REM pass/fail/crash summary that names the suites needing attention.
REM
REM Usage:
REM   regression_tests.cmd [jobs] [path-to-cajeta_test.exe]
REM     jobs                concurrent workers   (default: %NUMBER_OF_PROCESSORS%)
REM     path-to-cajeta_test test binary          (default: build\test\cajeta_test.exe)
REM
REM Exit code: 0 if every suite is clean; 1 if any failures or crashes.

set "ROOT=%~dp0"
pushd "%ROOT%"

set "JOBS=%~1"
if "%JOBS%"=="" set "JOBS=%NUMBER_OF_PROCESSORS%"
set "EXE=%~2"
if "%EXE%"=="" set "EXE=build\test\cajeta_test.exe"
if not exist "%EXE%" (
    echo error: test binary not found: %EXE%
    echo        build first ^(cmake --build build^) or pass the path as arg 2.
    popd & exit /b 2
)

set "OUT=%TEMP%\cajeta_sweep"
set "LOGS=%OUT%\logs"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%LOGS%" 2>nul

echo listing suites...
"%EXE%" --gtest_list_tests > "%OUT%\listing.txt" 2>nul
REM Suite lines start at column 0 and end with a dot; test lines are indented.
findstr /R /C:"^[a-zA-Z].*\.$" "%OUT%\listing.txt" > "%OUT%\suites_raw.txt"
> "%OUT%\suites.txt" (
    for /f "usebackq tokens=1 delims= " %%S in ("%OUT%\suites_raw.txt") do (
        set "name=%%S"
        if "!name:~-1!"=="." set "name=!name:~0,-1!"
        echo !name!
    )
)

set /a NSUITES=0
for /f "usebackq delims=" %%S in ("%OUT%\suites.txt") do set /a NSUITES+=1
echo fanning !NSUITES! suites across %JOBS% workers (binary: %EXE%)
echo.

REM Launch each suite in the background, throttling so no more than JOBS run
REM at once (counted via tasklist).
for /f "usebackq delims=" %%S in ("%OUT%\suites.txt") do (
    call :throttle
    start "" /b cmd /c ""%EXE%" --gtest_filter=%%S.* > "%LOGS%\%%S.log" 2>&1"
)

REM Wait for the final batch to drain.
:waitdone
call :count_running
if !RUNNING! GTR 0 (
    ping -n 2 127.0.0.1 >nul
    goto :waitdone
)

REM Aggregate. A suite that never printed the gtest "[  PASSED  ]" summary
REM line crashed before completing.
set /a TP=0, TF=0, NCRASH=0, NFAILS=0
if exist "%OUT%\report.txt" del "%OUT%\report.txt"
for /f "usebackq delims=" %%S in ("%OUT%\suites.txt") do (
    set "log=%LOGS%\%%S.log"
    findstr /C:"[  PASSED  ]" "!log!" >nul 2>&1
    if errorlevel 1 (
        set /a NCRASH+=1
        >> "%OUT%\report.txt" echo   %%-46S CRASHED ^(no completion^)
    ) else (
        set "passed=0" & set "failed=0"
        for /f "tokens=4" %%a in ('findstr /C:"[  PASSED  ]" "!log!" 2^>nul') do set "passed=%%a"
        REM Non-brief gtest prints each failure twice (timed inline result +
        REM bare summary listing). Count only the timed inline line.
        for /f %%a in ('findstr /R /C:"^\[  FAILED  \].*([0-9]* ms)$" "!log!" 2^>nul ^| find /c /v ""') do set "failed=%%a"
        set /a "TP+=passed"
        set /a "TF+=failed"
        if !failed! GTR 0 (
            set /a NFAILS+=1
            >> "%OUT%\report.txt" echo   %%-46S !failed! failed
        )
    )
)

echo ================= REGRESSION SWEEP =================
echo suites: !NSUITES!      tests passed: !TP!      tests failed: !TF!
echo suites w/ failures: !NFAILS!     crashed processes: !NCRASH!
if exist "%OUT%\report.txt" (
    echo.
    echo -- suites needing attention --
    type "%OUT%\report.txt"
)
echo ===================================================
echo.
echo per-suite logs: %LOGS%

set /a BAD=NFAILS+NCRASH
popd
if !BAD! GTR 0 ( exit /b 1 ) else ( exit /b 0 )

:throttle
call :count_running
if !RUNNING! GEQ %JOBS% (
    ping -n 2 127.0.0.1 >nul
    goto :throttle
)
goto :eof

:count_running
set "RUNNING=0"
for /f %%C in ('tasklist /fi "imagename eq cajeta_test.exe" /nh 2^>nul ^| find /c "cajeta_test.exe"') do set "RUNNING=%%C"
goto :eof
