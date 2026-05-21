# Install the xxhash development headers needed by runtime/native/cajeta_runtime.c
# (it does `#define XXH_INLINE_ALL` then `#include <xxhash.h>`, so only the
# headers are required — no .lib link step).
#
# Strategy, in order:
#   1. vcpkg  — the natural fit for a CMake C++ project on Windows.
#   2. winget — Microsoft's first-party package manager.
#   3. choco  — Chocolatey.
#   4. source — fetch the header set from the upstream GitHub release.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\install-xxhash-windows.ps1
#
# Override the source-fallback drop location with -InstallPrefix or
# $env:XXHASH_PREFIX (defaults to %ProgramFiles%\xxhash).

[CmdletBinding()]
param(
    [string] $XxhashVersion = $(if ($env:XXHASH_VERSION) { $env:XXHASH_VERSION } else { '0.8.3' }),
    [string] $InstallPrefix = $(if ($env:XXHASH_PREFIX) { $env:XXHASH_PREFIX } else { Join-Path $env:ProgramFiles 'xxhash' })
)

$ErrorActionPreference = 'Stop'

function Test-Cmd([string] $name) {
    return [bool](Get-Command -Name $name -ErrorAction SilentlyContinue)
}

function Try-Vcpkg {
    # Prefer an explicit VCPKG_ROOT, otherwise fall back to `vcpkg` on PATH.
    $vcpkgExe = $null
    if ($env:VCPKG_ROOT) {
        $candidate = Join-Path $env:VCPKG_ROOT 'vcpkg.exe'
        if (Test-Path $candidate) { $vcpkgExe = $candidate }
    }
    if (-not $vcpkgExe -and (Test-Cmd 'vcpkg')) { $vcpkgExe = 'vcpkg' }
    if (-not $vcpkgExe) { return $false }

    Write-Host "Detected vcpkg — installing xxhash."
    & $vcpkgExe install xxhash
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "vcpkg install xxhash exited with code $LASTEXITCODE."
        return $false
    }
    Write-Host ""
    Write-Host "Wire vcpkg into your CMake configure step with:"
    Write-Host "  cmake -B build -DCMAKE_TOOLCHAIN_FILE=`$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
    return $true
}

function Try-Winget {
    if (-not (Test-Cmd 'winget')) { return $false }
    # xxhash is not consistently in the winget catalog; this is best-effort.
    Write-Host "Detected winget — attempting to install xxhash."
    & winget install --id Cyan4973.xxHash -e --accept-source-agreements --accept-package-agreements 2>$null
    if ($LASTEXITCODE -eq 0) { return $true }
    Write-Warning "winget did not find a matching xxhash package."
    return $false
}

function Try-Choco {
    if (-not (Test-Cmd 'choco')) { return $false }
    Write-Host "Detected Chocolatey — attempting to install xxhash."
    & choco install xxhash -y
    return ($LASTEXITCODE -eq 0)
}

function Try-Source {
    Write-Host "Falling back to source download of xxhash $XxhashVersion."
    $tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP ("xxhash-" + [Guid]::NewGuid().ToString('N')))
    try {
        $tarball = Join-Path $tmp.FullName 'xxhash.tar.gz'
        $url = "https://github.com/Cyan4973/xxHash/archive/refs/tags/v$XxhashVersion.tar.gz"
        Invoke-WebRequest -Uri $url -OutFile $tarball -UseBasicParsing

        # tar.exe ships with Windows 10 1803+ / Windows 11.
        if (-not (Test-Cmd 'tar')) {
            throw "tar.exe not found. Either install a recent Windows 10/11 build or extract $tarball manually."
        }
        & tar -xzf $tarball -C $tmp.FullName
        if ($LASTEXITCODE -ne 0) { throw "tar failed to extract $tarball." }

        $src = Join-Path $tmp.FullName "xxHash-$XxhashVersion"
        $includeDir = Join-Path $InstallPrefix 'include'
        New-Item -ItemType Directory -Force -Path $includeDir | Out-Null
        foreach ($h in @('xxhash.h', 'xxh3.h', 'xxh_x86dispatch.h')) {
            Copy-Item -Force (Join-Path $src $h) (Join-Path $includeDir $h)
        }
        Write-Host "Installed xxhash headers to $includeDir."
        Write-Host ""
        Write-Host "Make sure your compiler can find them. For MSVC + CMake:"
        Write-Host "  cmake -B build -DCMAKE_C_FLAGS=`"/I`"$includeDir`"`""
        Write-Host "Or for clang/MinGW:"
        Write-Host "  set CPATH=$includeDir;%CPATH%"
    }
    finally {
        Remove-Item -Recurse -Force $tmp.FullName -ErrorAction SilentlyContinue
    }
}

if (-not (Try-Vcpkg)) {
    if (-not (Try-Winget)) {
        if (-not (Try-Choco)) {
            Try-Source
        }
    }
}

Write-Host ""
Write-Host "Done. If a downstream build still can't resolve <xxhash.h>, add the"
Write-Host "appropriate include directory to your compiler's search path."
