@echo off
setlocal EnableExtensions

rem ===========================================================================
rem cajeta Windows setup -- analogue of setup.sh for native Windows.
rem
rem Installs the MSYS2 / MinGW-w64 toolchain + libraries and configures the
rem CMake build under .\build. Re-running is idempotent (winget and pacman
rem skip already-installed packages; cmake re-configures in place).
rem
rem The Windows build uses MSYS2 / MinGW-w64 (NOT MSVC): pacman ships pre-built
rem mingw-w64 packages for LLVM, lld, antlr4-runtime, etc., and the near-POSIX
rem surface (winpthreads, libucontext) lets cajeta_runtime.c compile with
rem minimal conditionals. Resulting binary is native PE; triple is
rem x86_64-w64-mingw32. This mirrors .github/workflows/release.yml.
rem
rem Knobs (set before running):
rem   MSYS2_ROOT   - MSYS2 install root (default C:\msys64).
rem ===========================================================================

rem Work from the repo root (this script lives there).
cd /d "%~dp0"

if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
set "BASH=%MSYS2_ROOT%\usr\bin\bash.exe"

rem Make the MinGW64 bash inherit the full Windows PATH so a Windows-installed
rem JDK (needed to run the ANTLR4 codegen jar at build time) is visible. The
rem mingw64 / usr dirs are still prepended, so the toolchain wins over any
rem same-named Windows tools.
set "MSYSTEM=MINGW64"
set "MSYS2_PATH_TYPE=inherit"
set "CHERE_INVOKING=1"

rem --- Locate / install MSYS2 ------------------------------------------------
if not exist "%BASH%" (
    echo [setup] MSYS2 not found at %MSYS2_ROOT% -- installing via winget...
    where winget >nul 2>&1
    if errorlevel 1 (
        echo [setup] ERROR: winget unavailable. Install MSYS2 from https://www.msys2.org/
        echo         then re-run, or set MSYS2_ROOT to an existing install.
        exit /b 1
    )
    winget install --id MSYS2.MSYS2 -e --accept-package-agreements --accept-source-agreements --disable-interactivity
    if not exist "%BASH%" (
        echo [setup] ERROR: MSYS2 did not install at %MSYS2_ROOT%.
        echo         Set MSYS2_ROOT to the actual location and re-run.
        exit /b 1
    )
)
echo [setup] Using MSYS2 at %MSYS2_ROOT%

rem --- Java (runs the ANTLR4 codegen jar during the build) ------------------
where java >nul 2>&1
if errorlevel 1 (
    echo [setup] Java not found -- installing Temurin 17 JDK via winget...
    where winget >nul 2>&1
    if errorlevel 1 (
        echo [setup] WARNING: winget unavailable and no java on PATH. Install a JDK 17+
        echo          before building, or ANTLR codegen will fail.
    ) else (
        winget install --id EclipseAdoptium.Temurin.17.JDK -e --accept-package-agreements --accept-source-agreements --disable-interactivity
        echo [setup] NOTE: open a NEW terminal so PATH picks up java, then re-run this script.
    )
)

rem --- Install MinGW-w64 toolchain + libraries via pacman -------------------
rem Mirrors the install list in .github/workflows/release.yml. `vim` is for
rem xxd, used to embed the runtime bitcode (src/CMakeLists.txt CAJETA_XXD).
echo [setup] Installing MinGW-w64 packages via pacman (this can take a while)...
"%BASH%" -lc "pacman -Sy --noconfirm && pacman -S --needed --noconfirm mingw-w64-x86_64-clang mingw-w64-x86_64-llvm mingw-w64-x86_64-lld mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-antlr4-runtime-cpp mingw-w64-x86_64-zstd mingw-w64-x86_64-xxhash mingw-w64-x86_64-glog mingw-w64-x86_64-gtest mingw-w64-x86_64-openssl vim"
if errorlevel 1 (
    echo [setup] ERROR: pacman package install failed.
    exit /b 1
)

rem --- Configure the CMake build (deps already installed above) -------------
rem Reuse setup.sh's portable configure path (LLVM_DIR=/mingw64/lib/cmake/llvm,
rem -G Ninja). CAJETA_SKIP_DEPS=1 skips its Unix dep-install phase.
echo [setup] Configuring CMake build under .\build ...
"%BASH%" -lc "CAJETA_SKIP_DEPS=1 ./setup.sh"
if errorlevel 1 (
    echo [setup] ERROR: CMake configure failed.
    exit /b 1
)

echo.
echo [setup] Done. To build, run:
echo     setup.cmd has configured .\build -- build it from an MSYS2 MINGW64 shell:
echo         cmake --build build -- -j 4
echo     or directly from cmd / PowerShell:
echo         "%MSYS2_ROOT%\usr\bin\bash.exe" -lc "cmake --build build -- -j 4"
echo     (this script already exported MSYSTEM=MINGW64 and MSYS2_PATH_TYPE=inherit
echo      so the toolchain and a Windows JDK are both visible).
endlocal
