@echo off
setlocal EnableExtensions
rem ===========================================================================
rem Build Cajeta on native Windows -- analogue of build.sh.
rem
rem Puts the MinGW-w64 toolchain on PATH and runs the CMake/Ninja build under
rem .\build. With no args it builds everything (the default `all` target);
rem any args are passed straight through to `cmake --build` (e.g. a target).
rem
rem Examples:
rem   build.cmd                          ::  build everything
rem   build.cmd --target cajeta          ::  just the compiler
rem   build.cmd --target cajeta_test     ::  just the unit-test binary
rem   build.cmd --target cajeta_debug_test  ::  the debugger test target
rem   build.cmd -v                       ::  verbose (ninja command echo)
rem
rem Knobs (set before running, e.g. `set JOBS=4`):
rem   MSYS2_ROOT   MSYS2 install root (default C:\msys64); its mingw64\bin is
rem                prepended to PATH so cmake/clang/ninja AND the runtime DLLs
rem                (libstdc++, antlr4-runtime, ...) resolve.
rem   JOBS=N       parallel build jobs (default = %NUMBER_OF_PROCESSORS%).
rem ===========================================================================

rem Work from the repo root (this script lives there).
cd /d "%~dp0"

rem MinGW-w64 toolchain + runtime DLLs on PATH. Only mingw64\bin -- NOT usr\bin,
rem whose MSYS coreutils would shadow the Windows tools. The full Windows PATH is
rem kept after it so a Windows JDK (needed by the ANTLR4 codegen step) stays
rem visible.
if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
if not exist "%MSYS2_ROOT%\mingw64\bin\cmake.exe" (
    echo error: cmake not found under %MSYS2_ROOT%\mingw64\bin
    echo        Install the toolchain with setup.cmd, or set MSYS2_ROOT.
    exit /b 1
)
set "PATH=%MSYS2_ROOT%\mingw64\bin;%PATH%"

rem cajeta-llvm FORK toolchain (optional). cajeta's Tier-1 SPIR-V codegen references
rem intrinsics that only exist in the cajeta-llvm fork (LLVM 23); stock MSYS2 LLVM 22
rem can't compile it. To build locally against the fork, download/extract the
rem windows-x64 fork artifact (built by plans/c0/fork-build-llvm.yml's build-windows
rem job) and point CAJETA_LLVM_ROOT at its root (the dir containing bin/ and
rem lib/cmake/llvm). Its bin\ is prepended AHEAD of mingw64\bin so the fork's clang /
rem llvm-config win, and LLVM_DIR is exported so the (re)configure resolves find_package
rem to the fork. Leave it unset to use stock MSYS2 LLVM (Tier-1 SPIR-V kernels then fail
rem to build until the fork is supplied).
if defined CAJETA_LLVM_ROOT (
    if not exist "%CAJETA_LLVM_ROOT%\lib\cmake\llvm" (
        echo error: CAJETA_LLVM_ROOT set but %CAJETA_LLVM_ROOT%\lib\cmake\llvm not found
        exit /b 1
    )
    set "PATH=%CAJETA_LLVM_ROOT%\bin;%PATH%"
    rem setup.sh / setup.cmd read LLVM_DIR; forward-slash form for CMake.
    set "LLVM_DIR=%CAJETA_LLVM_ROOT:\=/%/lib/cmake/llvm"
    rem Build cajeta's C++ with mingw g++ (matching CI + the stock build), NOT the
    rem fork's clang++. The fork supplies LLVM libs/headers + clang for the runtime-
    rem bitcode step only. Putting its bin first on PATH makes CMake auto-pick the
    rem fork clang++ as the C++ compiler, which on mingw doesn't auto-link
    rem winpthreads -> LLVM/libstdc++ pthread_* undefined-reference link failures.
    rem gcc/g++ resolve to mingw64\bin (the fork ships no gcc/g++).
    set "CC=gcc"
    set "CXX=g++"
    echo ^>^> Using cajeta-llvm fork toolchain at %CAJETA_LLVM_ROOT% (compiler: mingw g++)
)

rem Configure the build tree on first use (setup.cmd installs deps + runs cmake).
if not exist "build\build.ninja" (
    echo ^>^> No build\ found, running setup.cmd
    call setup.cmd || ( echo error: setup.cmd failed & exit /b 1 )
)

if not defined JOBS set "JOBS=%NUMBER_OF_PROCESSORS%"
if not defined JOBS set "JOBS=4"

echo ^>^> cmake --build build -j %JOBS% %*
cmake --build build -j %JOBS% %* || ( echo error: build failed & exit /b 1 )

echo ^>^> build OK
endlocal
