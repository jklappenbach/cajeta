@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ===========================================================================
rem Run Cajeta tests on native Windows -- analogue of cajeta_tests.sh.
rem
rem No args = full suite (parallel across cores, compact summary); otherwise
rem each arg is a test filter pattern (suite name, full test name, or wildcard)
rem run serially with raw gtest output. Anything beginning with `--` is passed
rem straight to the test binary, so any gtest flag works. The lone arg `stop`
rem kills every running cajeta test.
rem
rem Examples:
rem   cajeta_tests.cmd                              ::  everything (parallel)
rem   cajeta_tests.cmd BinaryOpTests                ::  whole suite (serial)
rem   cajeta_tests.cmd BinaryOpTests.intAdd         ::  one test (serial)
rem   cajeta_tests.cmd BinaryOpTests CompareTests   ::  multiple suites (serial)
rem   cajeta_tests.cmd Fp*                          ::  gtest wildcard (serial)
rem   cajeta_tests.cmd --gtest_brief=0              ::  raw passthrough for flags
rem   cajeta_tests.cmd stop                         ::  kill every running test/shard
rem
rem Knobs (set before running, e.g. `set NO_BUILD=1`):
rem   NO_BUILD=1   skip the incremental build step
rem   PARALLEL=0   force serial run even without filters
rem   PARALLEL=1   force parallel run even with filters
rem   PARALLEL=N   use N shards instead of the CPU count
rem   VERBOSE=1    in parallel mode, dump each shard's full gtest output
rem   NO_RETRY=1   skip the serial retry of failed/crashed shards (first pass
rem                is authoritative; otherwise problem shards are re-run alone
rem                and only deterministic failures count)
rem   MSYS2_ROOT   MSYS2 install root (default C:\msys64); its mingw64\bin is
rem                prepended to PATH so cmake/ninja AND the test binary's
rem                runtime DLLs (libstdc++, antlr4-runtime, ...) resolve.
rem ===========================================================================

rem Work from the repo root (this script lives there).
cd /d "%~dp0"
set "ROOT=%CD%"

rem `stop` subcommand: kill every running cajeta test process. All shards are
rem cajeta_test.exe image instances, so a single taskkill by image name clears
rem direct runs and parallel shards alike. Handled before the build so it never
rem triggers a compile.
if /i "%~1"=="stop" (
    echo ^>^> Stopping all running cajeta tests...
    taskkill /F /IM cajeta_test.exe /T >nul 2>&1
    if errorlevel 1 (
        echo ^>^> No running cajeta tests found.
    ) else (
        echo ^>^> Stopped running cajeta_test.exe process^(es^).
    )
    exit /b 0
)

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

rem --- cajeta-llvm fork dylib on PATH ----------------------------------------
rem The dylib slim-down links cajeta_test.exe against libLLVM-<ver>.dll, so that
rem dll's directory must be on PATH at RUN time (Windows resolves DLLs via PATH /
rem the exe's dir, not an rpath). build.cmd prepends it when CAJETA_LLVM_ROOT is
rem set, but tests are usually run in a shell WITHOUT that env var -- so also
rem derive the dir from the configured build's LLVM_DIR. Skip silently for a stock
rem (non-dylib) build. Without this the binary fails to even launch, with exit
rem 0xC0000135 / -1073741515 (STATUS_DLL_NOT_FOUND).
set "LLVM_BIN="
if defined CAJETA_LLVM_ROOT if exist "%CAJETA_LLVM_ROOT%\bin\libLLVM*.dll" set "LLVM_BIN=%CAJETA_LLVM_ROOT%\bin"
if not defined LLVM_BIN if exist "build\CMakeCache.txt" (
    for /f "usebackq tokens=2 delims==" %%D in (`findstr /B /C:"LLVM_DIR:" "build\CMakeCache.txt"`) do set "LLVM_DIR_RAW=%%D"
    if defined LLVM_DIR_RAW (
        set "LLVM_DIR_RAW=!LLVM_DIR_RAW:/=\!"
        set "LLVM_BIN=!LLVM_DIR_RAW:\lib\cmake\llvm=!\bin"
    )
)
if defined LLVM_BIN if exist "!LLVM_BIN!\libLLVM*.dll" set "PATH=!LLVM_BIN!;%PATH%"

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
rem Serial path -- mirror cajeta_tests.sh's behavior verbatim.
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
rem      with PARALLEL=N). The 32 cap mirrors cajeta_tests.sh -- LLJIT memory
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
rem When filter patterns are present (e.g. release_tests.cmd delegates a
rem curated subset here with PARALLEL forced), restrict discovery to those
rem patterns so the shards cover only the selected tests. With no patterns
rem listfilter stays empty and every test is discovered as before.
set "listfilter="
for /l %%i in (1,1,%npat%) do (
    set "p=!pat_%%i!"
    set "hasdot="
    set "hasstar="
    echo(!p!| findstr /C:"." >nul && set "hasdot=1"
    echo(!p!| findstr /C:"*" >nul && set "hasstar=1"
    if not defined hasdot if not defined hasstar set "p=!p!.*"
    if defined listfilter ( set "listfilter=!listfilter!:!p!" ) else ( set "listfilter=!p!" )
)
set "lfarg="
if defined listfilter set "lfarg=--gtest_filter=!listfilter!"
"%TEST_BIN%" --gtest_list_tests !lfarg! > "%TMPD%\list.txt" 2>nul

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

rem Round-robin distribute test names into per-shard filter strings. tot_<s>
rem tracks how many tests each shard owns, so the live dashboard can show
rem per-shard completion.
for /l %%s in (0,1,%last%) do ( set "sf_%%s=" & set "tot_%%s=0" )
for /l %%i in (1,1,%ntests%) do (
    set /a "s=(%%i-1) %% shards"
    for %%S in (!s!) do (
        if defined sf_%%S ( set "sf_%%S=!sf_%%S!:!test_%%i!" ) else ( set "sf_%%S=!test_%%i!" )
        set /a "tot_%%S+=1"
    )
)

rem Live dashboard setup (Windows 10+ consoles with VT processing). An
rem interactive parallel run draws an overall bar + one row per shard, repainted
rem on the alternate screen via VT escapes. Disable with TUI=0; VERBOSE=1 also
rem forces the plain path. The live view needs the per-test [ RUN ]/[ OK ] lines
rem that --gtest_brief=1 suppresses, so shards emit full output when live.
set "LIVE=1"
if defined TUI if "%TUI%"=="0" set "LIVE=0"
if defined VERBOSE if "%VERBOSE%"=="1" set "LIVE=0"
set "BRIEF=--gtest_brief=1"
if "%LIVE%"=="1" set "BRIEF=--gtest_brief=0"
rem ESC control character for the VT sequences below.
for /f %%a in ('echo prompt $E ^| cmd') do set "ESC=%%a"

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
    >>"!scmd!" echo "%ROOT%\%TEST_BIN%" --gtest_filter=!sf_%%s! !BRIEF! ^> "!out!" 2^>^&1
    rem Wrap the echo in parens so a bare-digit errorlevel is not parsed as a
    rem redirection by the child cmd when it records its exit code.
    >>"!scmd!" echo ^(echo %%errorlevel%%^)^> "!exitf!"
    start "cajeta_shard_%%s" /b cmd /c "!scmd!"
)

rem Wait for every shard (its .exit file appears once the wrapper finishes).
rem When live, repaint the dashboard each poll on the alternate screen.
if "%LIVE%"=="1" <nul set /p "=%ESC%[?1049h%ESC%[?25l%ESC%[2J"
:poll
set "pending="
for /l %%s in (0,1,%last%) do if not exist "%TMPD%\shard_%%s.exit" set "pending=1"
if "%LIVE%"=="1" call :render
if defined pending ( ping -n 2 127.0.0.1 >nul & goto poll )
rem Leave the alternate screen so the summary prints on the normal screen.
if "%LIVE%"=="1" <nul set /p "=%ESC%[?25h%ESC%[?1049l"

call :secs "%TIME%" T1
set /a elapsed=T1-T0
if %elapsed% LSS 0 set /a elapsed+=86400

rem Aggregate. :agg also records every shard that failed or crashed into
rem retry_list (space-separated indices) for the serial retry pass below.
set /a total_pass=0
set /a total_fail=0
set /a ncrash=0
set /a nretry=0
set "retry_list="
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
    echo   set PARALLEL=0 ^&^& cajeta_tests.cmd ^<SuiteName.testName^>
)

if "%VERBOSE%"=="1" (
    echo.
    echo === Per-shard output ===
    for /l %%s in (0,1,%last%) do (
        echo ----- shard %%s -----
        type "%TMPD%\shard_%%s.out"
    )
)

rem ---------------------------------------------------------------------------
rem Retry pass. Running 32 LLJIT shards at once puts the box under enough
rem memory/scheduling pressure that a shard or two randomly takes an access
rem violation (or a JIT null-return -> wrong value -> assertion fail). The
rem named test just happens to be the last one that started; it moves run to
rem run. Re-run each problem shard ON ITS OWN (serial, no contention): genuine
rem deterministic failures reproduce and stay red, flaky ones recover. The
rem final exit code is based on the retry, not the first pass. Set
rem NO_RETRY=1 to skip and treat the first pass as authoritative.
rem ---------------------------------------------------------------------------
set "had_problems="
if %total_fail% GTR 0 set "had_problems=1"
if %ncrash% GTR 0 set "had_problems=1"

if not defined had_problems (
    rd /s /q "%TMPD%" 2>nul
    exit /b 0
)

if defined NO_RETRY (
    rd /s /q "%TMPD%" 2>nul
    if %total_fail% GTR 0 exit /b 1
    if %ncrash% GTR 0 exit /b 1
    exit /b 0
)

echo.
echo === Retry pass: re-running !nretry! problem shard^(s^) serially ===
echo ^(memory-pressure flakiness recovers here; deterministic failures persist^)
for %%s in (!retry_list!) do (
    echo   re-running shard %%s ...
    cmd /c "%TMPD%\shard_%%s.cmd"
)

set /a r_pass=0
set /a r_fail=0
set /a r_crash=0
del "%TMPD%\retry_failures.txt" 2>nul
del "%TMPD%\retry_crashes.txt" 2>nul
for %%s in (!retry_list!) do call :ragg %%s

echo.
echo === Retry summary ===
echo Retried shards: !nretry!   Still failing: !r_fail!   Still crashing: !r_crash!
if exist "%TMPD%\retry_failures.txt" (
    echo.
    echo Persisting failures ^(real^):
    for /f "usebackq tokens=* delims=" %%a in ("%TMPD%\retry_failures.txt") do echo   %%a
)
if exist "%TMPD%\retry_crashes.txt" (
    echo.
    echo Persisting crashes ^(real^):
    type "%TMPD%\retry_crashes.txt"
)

rd /s /q "%TMPD%" 2>nul

if !r_fail! GTR 0 exit /b 1
if !r_crash! GTR 0 exit /b 1
echo.
echo All problem shards passed on serial retry -- the first-pass failures were
echo memory-pressure flakiness, not real failures. PASS.
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
for /f %%a in ('findstr /R /C:"^\[  FAILED  \] [a-zA-Z_].* ms)$" "%out%" 2^>nul ^| find /c /v ""') do set "failed=%%a"

set /a total_pass+=passed
set /a total_fail+=failed
if %failed% GTR 0 findstr /R /C:"^\[  FAILED  \] [a-zA-Z_].* ms)$" "%out%" >> "%TMPD%\failures.txt" 2>nul
if %failed% GTR 0 (
    set /a nretry+=1
    set "retry_list=!retry_list! %s%"
)

rem Non-zero exit with no counted failure == crashed mid-run. Re-run the shard
rem under --gtest_brief=0 to surface the [ RUN ] sequence and name the last
rem test that started before the crash.
set "crashed="
if not "%ec%"=="0" if %failed%==0 set "crashed=1"
if defined crashed (
    set /a ncrash+=1
    set /a nretry+=1
    set "retry_list=!retry_list! %s%"
    "%ROOT%\%TEST_BIN%" --gtest_filter=!sf_%s%! --gtest_brief=0 > "%TMPD%\v_%s%.txt" 2>nul
    set "last_run=<none>"
    for /f "tokens=4" %%a in ('findstr /C:"[ RUN      ]" "%TMPD%\v_%s%.txt" 2^>nul') do set "last_run=%%a"
    call :reason "%ec%"
    >> "%TMPD%\crashes.txt" echo   shard %s%: !rsn!; last test started: !last_run!
)
goto :eof

rem ---------------------------------------------------------------------------
rem :ragg <shard-index> -- re-aggregate ONE retried shard into the r_* totals.
rem Mirrors :agg but writes to the retry tallies and retry_{failures,crashes}.
rem The shard's wrapper .cmd has already been re-run serially, overwriting its
rem .out/.exit, so this just re-parses the fresh result.
rem ---------------------------------------------------------------------------
:ragg
set "s=%~1"
set "out=%TMPD%\shard_%s%.out"
set "ec="
set /p ec=<"%TMPD%\shard_%s%.exit"
if not defined ec set "ec=?"

set "failed=0"
for /f %%a in ('findstr /R /C:"^\[  FAILED  \] [a-zA-Z_].* ms)$" "%out%" 2^>nul ^| find /c /v ""') do set "failed=%%a"
set /a r_fail+=failed
if %failed% GTR 0 findstr /R /C:"^\[  FAILED  \] [a-zA-Z_].* ms)$" "%out%" >> "%TMPD%\retry_failures.txt" 2>nul

set "crashed="
if not "%ec%"=="0" if %failed%==0 set "crashed=1"
if defined crashed (
    set /a r_crash+=1
    "%ROOT%\%TEST_BIN%" --gtest_filter=!sf_%s%! --gtest_brief=0 > "%TMPD%\rv_%s%.txt" 2>nul
    set "last_run=<none>"
    for /f "tokens=4" %%a in ('findstr /C:"[ RUN      ]" "%TMPD%\rv_%s%.txt" 2^>nul') do set "last_run=%%a"
    call :reason "%ec%"
    >> "%TMPD%\retry_crashes.txt" echo   shard %s%: !rsn!; last test started: !last_run!
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

rem ---------------------------------------------------------------------------
rem :render -- repaint the live dashboard (overall bar + one row per shard) on
rem the alternate screen. Per shard, done = its `[ OK ]` + inline
rem `[ FAILED ] ... (N ms)` lines; current test = the last `[ RUN ]` line. VT
rem escapes home the cursor and clear each line so the block repaints in place.
rem Windows 10+ consoles with virtual-terminal processing only (TUI=0 opts out).
rem Two passes: sum first (the header bar needs the total), then draw the rows.
rem ---------------------------------------------------------------------------
:render
set /a rtotal=0
for /l %%s in (0,1,%last%) do (
    set "ro=%TMPD%\shard_%%s.out"
    set "rd=0"
    for /f %%a in ('findstr /R /C:"^\[ *OK *\]" "!ro!" 2^>nul ^| find /c /v ""') do set "rd=%%a"
    for /f %%a in ('findstr /R /C:"^\[  FAILED  \] [a-zA-Z_].* ms)$" "!ro!" 2^>nul ^| find /c /v ""') do set /a "rd+=%%a"
    set "rd_%%s=!rd!"
    set /a "rtotal+=rd"
)
set /a "hfill=0"
if %ntests% GTR 0 set /a "hfill=rtotal*34/ntests"
if !hfill! GTR 34 set "hfill=34"
set "hbar="
for /l %%b in (1,1,34) do if %%b LEQ !hfill! (set "hbar=!hbar!#") else (set "hbar=!hbar!-")
call :secs "%TIME%" TN
set /a "rel=TN-T0"
if !rel! LSS 0 set /a "rel+=86400"
<nul set /p "=%ESC%[H"
echo %ESC%[KCajeta tests [!hbar!] !rtotal!/%ntests%  %shards% shards  !rel!s
echo %ESC%[K
rem No per-shard progress bar: gtest gives no progress inside a single test, and
rem a shard runs one test at a time, so a completion bar barely moves. Show the
rem running test and how long it has been going (live seconds), with the shard's
rem done/assigned count alongside.
for /l %%s in (0,1,%last%) do (
    set "rd=!rd_%%s!"
    set "rtot=!tot_%%s!"
    if not defined rtot set "rtot=0"
    if exist "%TMPD%\shard_%%s.exit" (
        echo %ESC%[Kshard %%s  !rd!/!rtot!  done
    ) else (
        set "ro=%TMPD%\shard_%%s.out"
        set "rcur=starting"
        for /f "tokens=4" %%a in ('findstr /C:"[ RUN      ]" "!ro!" 2^>nul') do set "rcur=%%a"
        set "rcur=!rcur:~0,44!"
        rem Reset the per-test stopwatch when the running test changes.
        if not "!last_%%s!"=="!rcur!" ( set "last_%%s=!rcur!" & set "since_%%s=!TN!" )
        set /a "rtel=TN-since_%%s"
        if !rtel! LSS 0 set /a "rtel+=86400"
        echo %ESC%[Kshard %%s  !rd!/!rtot!  !rcur! !rtel!s
    )
)
<nul set /p "=%ESC%[J"
goto :eof
