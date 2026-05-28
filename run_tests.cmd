@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ===========================================================================
rem Run Cajeta tests on native Windows -- analogue of run_tests.sh.
rem
rem No args = full suite (parallel across cores, compact summary); otherwise
rem each arg is a test filter pattern (suite name, full test name, or wildcard)
rem run serially with raw gtest output. Anything beginning with `--` is passed
rem straight to the test binary, so any gtest flag works.
rem
rem Examples:
rem   run_tests.cmd                              ::  everything (parallel)
rem   run_tests.cmd BinaryOpTests                ::  whole suite (serial)
rem   run_tests.cmd BinaryOpTests.intAdd         ::  one test (serial)
rem   run_tests.cmd BinaryOpTests CompareTests   ::  multiple suites (serial)
rem   run_tests.cmd Fp*                          ::  gtest wildcard (serial)
rem   run_tests.cmd --gtest_brief=0              ::  raw passthrough for flags
rem
rem Knobs (set before running, e.g. `set NO_BUILD=1`):
rem   NO_BUILD=1   skip the incremental build step
rem   PARALLEL=0   force serial run even without filters
rem   PARALLEL=1   force parallel run even with filters
rem   PARALLEL=N   use N shards instead of the CPU count
rem   VERBOSE=1    in parallel mode, dump each shard's full gtest output
rem   MSYS2_ROOT   MSYS2 install root (default C:\msys64); its mingw64\bin is
rem                prepended to PATH so cmake/ninja AND the test binary's
rem                runtime DLLs (libstdc++, antlr4-runtime, ...) resolve.
rem ===========================================================================

rem Work from the repo root (this script lives there).
cd /d "%~dp0"
set "ROOT=%CD%"

rem MinGW-w64 toolchain + runtime DLLs on PATH. Only mingw64\bin -- NOT
rem usr\bin, whose MSYS coreutils (find.exe, sort.exe, ...) would shadow the
rem Windows tools this script relies on.
if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
if exist "%MSYS2_ROOT%\mingw64\bin" set "PATH=%MSYS2_ROOT%\mingw64\bin;%PATH%"

set "TEST_BIN=build\test\cajeta_test.exe"

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

rem --- Split args: `--*` are gtest flags, the rest are filter patterns --------
set "flags="
set "npat=0"
:argloop
if "%~1"=="" goto argsdone
set "arg=%~1"
if "!arg:~0,2!"=="--" (
    set "flags=!flags! %~1"
) else (
    set /a npat+=1
    set "pat_!npat!=%~1"
)
shift
goto argloop
:argsdone

rem --- Decide parallel vs serial ---------------------------------------------
rem Default: parallel when there are no filter patterns. Flags that change
rem discovery (--gtest_list_tests / --gtest_filter) force serial. PARALLEL
rem overrides everything.
set "should=0"
if %npat%==0 set "should=1"
echo !flags! | findstr /C:"--gtest_list_tests" >nul && set "should=0"
echo !flags! | findstr /C:"--gtest_filter=" >nul && set "should=0"

set "shards=%NUMBER_OF_PROCESSORS%"
if defined PARALLEL (
    if "!PARALLEL!"=="0" (
        set "should=0"
    ) else if "!PARALLEL!"=="1" (
        set "should=1"
    ) else (
        rem PARALLEL set to 2 or more means parallel with that many shards.
        set "nonnum="
        for /f "delims=0123456789" %%x in ("!PARALLEL!") do set "nonnum=1"
        if not defined nonnum if !PARALLEL! GTR 1 (
            set "should=1"
            set "shards=!PARALLEL!"
        )
    )
)

if "%should%"=="0" goto serial
goto parallel

rem ===========================================================================
rem Serial path -- mirror run_tests.sh's behavior verbatim.
rem ===========================================================================
:serial
set "filter="
for /l %%i in (1,1,%npat%) do (
    set "p=!pat_%%i!"
    rem Append `.*` to bare suite names so `BinaryOpTests` runs the whole suite.
    set "hasdot="
    set "hasstar="
    echo(!p!| findstr /C:"." >nul && set "hasdot=1"
    echo(!p!| findstr /C:"*" >nul && set "hasstar=1"
    if not defined hasdot if not defined hasstar set "p=!p!.*"
    if defined filter ( set "filter=!filter!:!p!" ) else ( set "filter=!p!" )
)

set "fflags=!flags!"
if defined filter set "fflags=!fflags! --gtest_filter=!filter!"
echo !fflags! | findstr /C:"--gtest_brief" >nul || set "fflags=!fflags! --gtest_brief=1"

echo ^>^> CAJETA_SOURCE_ROOT="%ROOT%" %TEST_BIN%!fflags!
"%TEST_BIN%"!fflags!
exit /b %errorlevel%

rem ===========================================================================
rem Parallel path:
rem   1. List every test via --gtest_list_tests.
rem   2. Round-robin into N buckets (N = CPU count, capped at 32; override
rem      with PARALLEL=N). The 32 cap mirrors run_tests.sh -- LLJIT memory
rem      pressure causes random crashes in a few shards beyond ~32-way.
rem   3. Spawn each bucket as a background process with its own --gtest_filter,
rem      writing output + exit code to temp files.
rem   4. Wait, parse each shard's gtest summary, aggregate, print a compact
rem      report. Surface crashing shards by name + last test that started.
rem   5. Exit non-zero if anything failed or crashed.
rem ===========================================================================
:parallel
if %shards% GTR 32 set "shards=32"

set "TMPD=%TEMP%\cajeta_test_shards_%RANDOM%%RANDOM%"
md "%TMPD%" 2>nul

echo ^>^> Discovering tests...
"%TEST_BIN%" --gtest_list_tests > "%TMPD%\list.txt" 2>nul

rem gtest prints one line per suite ending in `.`, then indented test names
rem (with optional ` # ...` type/value-param comments). tokens=1 strips the
rem indentation; a trailing `.` on the first token marks a suite header.
set "ntests=0"
set "cur="
for /f "usebackq tokens=1" %%L in ("%TMPD%\list.txt") do (
    set "tok=%%L"
    if "!tok:~0,1!"=="#" (
        rem skip a standalone type/value-param comment line
    ) else if "!tok:~-1!"=="." (
        set "cur=!tok!"
    ) else if defined cur (
        set /a ntests+=1
        set "test_!ntests!=!cur!!tok!"
    )
)

if %ntests%==0 ( echo error: no tests discovered via --gtest_list_tests & rd /s /q "%TMPD%" 2>nul & exit /b 1 )

rem Cap shards at the test count -- extra shards just sit empty.
if %ntests% LSS %shards% set "shards=%ntests%"
set /a last=shards-1

rem Round-robin distribute test names into per-shard filter strings.
for /l %%s in (0,1,%last%) do set "sf_%%s="
for /l %%i in (1,1,%ntests%) do (
    set /a s=(%%i-1) %% shards
    for %%S in (!s!) do (
        if defined sf_%%S ( set "sf_%%S=!sf_%%S!:!test_%%i!" ) else ( set "sf_%%S=!test_%%i!" )
    )
)

echo ^>^> Running %ntests% tests across %shards% shards...
call :secs "%TIME%" T0

rem Launch each shard via a generated wrapper .cmd (keeps the long filter and
rem its redirections out of the start command line). CAJETA_SOURCE_ROOT is
rem already in the environment, so the children inherit it.
for /l %%s in (0,1,%last%) do (
    set "out=%TMPD%\shard_%%s.out"
    set "exitf=%TMPD%\shard_%%s.exit"
    set "scmd=%TMPD%\shard_%%s.cmd"
    > "!scmd!" echo @echo off
    >>"!scmd!" echo "%ROOT%\%TEST_BIN%" --gtest_filter=!sf_%%s! --gtest_brief=1 ^> "!out!" 2^>^&1
    rem Wrap the echo in parens so a bare-digit errorlevel is not parsed as a
    rem redirection by the child cmd when it records its exit code.
    >>"!scmd!" echo ^(echo %%errorlevel%%^)^> "!exitf!"
    start "cajeta_shard_%%s" /b cmd /c "!scmd!"
)

rem Wait for every shard (its .exit file appears once the wrapper finishes).
:poll
set "pending="
for /l %%s in (0,1,%last%) do if not exist "%TMPD%\shard_%%s.exit" set "pending=1"
if defined pending ( ping -n 2 127.0.0.1 >nul & goto poll )

call :secs "%TIME%" T1
set /a elapsed=T1-T0
if %elapsed% LSS 0 set /a elapsed+=86400

rem Aggregate.
set /a total_pass=0
set /a total_fail=0
set /a ncrash=0
del "%TMPD%\failures.txt" 2>nul
del "%TMPD%\crashes.txt" 2>nul
for /l %%s in (0,1,%last%) do call :agg %%s

echo.
echo === Test summary ===
echo Discovered: %ntests%   Shards: %shards%   Elapsed: %elapsed%s
echo Passed: %total_pass%   Failed: %total_fail%   Crashed shards: %ncrash%

if exist "%TMPD%\failures.txt" (
    echo.
    echo Failed tests:
    for /f "usebackq tokens=* delims=" %%a in ("%TMPD%\failures.txt") do echo   %%a
)

if exist "%TMPD%\crashes.txt" (
    echo.
    echo Crashed shards:
    type "%TMPD%\crashes.txt"
    echo.
    echo Re-run the crashing test in isolation with:
    echo   set PARALLEL=0 ^&^& run_tests.cmd ^<SuiteName.testName^>
)

if "%VERBOSE%"=="1" (
    echo.
    echo === Per-shard output ===
    for /l %%s in (0,1,%last%) do (
        echo ----- shard %%s -----
        type "%TMPD%\shard_%%s.out"
    )
)

rd /s /q "%TMPD%" 2>nul

if %total_fail% GTR 0 exit /b 1
if %ncrash% GTR 0 exit /b 1
exit /b 0

rem ---------------------------------------------------------------------------
rem :agg <shard-index> -- parse one shard's output into the running totals.
rem ---------------------------------------------------------------------------
:agg
set "s=%~1"
set "out=%TMPD%\shard_%s%.out"
set "ec="
set /p ec=<"%TMPD%\shard_%s%.exit"
if not defined ec set "ec=?"

rem Passed count: --gtest_brief=1 still prints `[  PASSED  ] N tests.`.
set "passed=0"
for /f "tokens=4" %%a in ('findstr /C:"[  PASSED  ]" "%out%" 2^>nul') do set "passed=%%a"

rem Failed count: --gtest_brief=1 prints only per-test `[  FAILED  ] Suite.test`
rem lines (the next char after `] ` is an identifier char), not the numeric
rem count-summary line -- count those.
set "failed=0"
for /f %%a in ('findstr /R "^\[  FAILED  \] [a-zA-Z_]" "%out%" 2^>nul ^| find /c /v ""') do set "failed=%%a"

set /a total_pass+=passed
set /a total_fail+=failed
if %failed% GTR 0 findstr /R "^\[  FAILED  \] [a-zA-Z_]" "%out%" >> "%TMPD%\failures.txt" 2>nul

rem Non-zero exit with no counted failure == crashed mid-run. Re-run the shard
rem under --gtest_brief=0 to surface the [ RUN ] sequence and name the last
rem test that started before the crash.
set "crashed="
if not "%ec%"=="0" if %failed%==0 set "crashed=1"
if defined crashed (
    set /a ncrash+=1
    "%ROOT%\%TEST_BIN%" --gtest_filter=!sf_%s%! --gtest_brief=0 > "%TMPD%\v_%s%.txt" 2>nul
    set "last_run=<none>"
    for /f "tokens=4" %%a in ('findstr /C:"[ RUN      ]" "%TMPD%\v_%s%.txt" 2^>nul') do set "last_run=%%a"
    call :reason "%ec%"
    >> "%TMPD%\crashes.txt" echo   shard %s%: !rsn!; last test started: !last_run!
)
goto :eof

rem ---------------------------------------------------------------------------
rem :reason <exit-code> -- classify a shard's exit code into `rsn`. Windows
rem reports crashes as the NTSTATUS code (signed 32-bit) rather than POSIX
rem 128+signal, so map the common ones.
rem ---------------------------------------------------------------------------
:reason
set "code=%~1"
if "%code%"=="-1073741819" ( set "rsn=SIGSEGV-equiv: access violation (0xC0000005)"
) else if "%code%"=="-1073741795" ( set "rsn=illegal instruction (0xC000001D)"
) else if "%code%"=="-1073741676" ( set "rsn=integer divide-by-zero (0xC0000094)"
) else if "%code%"=="-1073740940" ( set "rsn=heap corruption (0xC0000374)"
) else if "%code%"=="-1073740791" ( set "rsn=stack buffer overrun / abort (0xC0000409)"
) else if "%code%"=="3" ( set "rsn=abort() (CRT exit 3)"
) else if "%code%"=="1" ( set "rsn=exit 1 with no gtest summary (aborted before report)"
) else if "%code%"=="?" ( set "rsn=exit code unrecorded (shard killed before writing)"
) else ( set "rsn=exit %code%" )
goto :eof

rem ---------------------------------------------------------------------------
rem :secs "<%TIME%>" <outvar> -- convert a clock time to seconds-since-midnight.
rem The 1-prefix trick avoids 08/09 being read as bad octal.
rem ---------------------------------------------------------------------------
:secs
setlocal
set "t=%~1"
set "t=%t: =0%"
for /f "tokens=1-3 delims=:.," %%a in ("%t%") do set /a "v=(((1%%a-100)*60)+(1%%b-100))*60+(1%%c-100)"
endlocal & set "%~2=%v%"
goto :eof
