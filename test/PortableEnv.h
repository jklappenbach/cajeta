//
// Portable setenv/unsetenv for tests.
//
// POSIX setenv(3)/unsetenv(3) don't exist in the MinGW/MSVCRT C library.
// Map them onto _putenv_s, which the CRT does provide. Setting a variable
// to the empty string removes it from the environment on Windows, so that
// doubles as unsetenv.
//

#pragma once

#ifdef _WIN32
#include <stdlib.h>
#include <process.h>   // _getpid

static inline int setenv(const char* name, const char* value, int /*overwrite*/) {
    return _putenv_s(name, value ? value : "");
}

static inline int unsetenv(const char* name) {
    return _putenv_s(name, "");
}
#else
#include <unistd.h>    // getpid
#endif

// Portable process id — used by tests to build unique temp paths. POSIX
// getpid(2) lives in <unistd.h> (absent on Windows); the CRT spells it _getpid.
static inline int cajeta_getpid() {
#ifdef _WIN32
    return _getpid();
#else
    return ::getpid();
#endif
}

// Portable path of the RUNNING TEST EXECUTABLE — used by tests that locate
// the build tree relative to themselves. `/proc/self/exe` is a Linux-ism:
// fs::canonical("/proc/self/exe") throws on macOS (no /proc) and Windows.
#include <filesystem>
#include <string>
#include <initializer_list>
#include <utility>
#if defined(__APPLE__)
#include <mach-o/dyld.h>   // _NSGetExecutablePath
#endif
// Windows deliberately needs NO extra include or declaration here:
// <windows.h> in a shared test header collides with std::byte (rpcndr.h
// 'byte' ambiguity) in using-namespace-std TUs, and re-declaring
// GetModuleFileNameA conflicts in TUs that DO include windows.h (HMODULE
// is not void*). The CRT's _get_pgmptr (already in <stdlib.h> above)
// hands back the executable path with no Win32 headers at all.

static inline std::filesystem::path cajeta_self_exe() {
#if defined(_WIN32)
    char* p = nullptr;
    if (_get_pgmptr(&p) != 0 || !p || !*p) return {};
    return std::filesystem::canonical(p);
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t sz = sizeof(buf);
    if (::_NSGetExecutablePath(buf, &sz) != 0) return {};
    return std::filesystem::canonical(buf);
#else
    return std::filesystem::canonical("/proc/self/exe");
#endif
}

// ---- Portable shell idioms for tests that std::system()/popen() a command --
// On MinGW, std::system() and popen() go through cmd.exe, which (a) has no
// /dev/null — `> /dev/null` fails with "The system cannot find the path
// specified" and the command's exit code is lost; (b) does not change drive
// on a bare `cd` — a test whose temp dir is on C: and whose build is on D:
// silently keeps running in the build dir; (c) has no `VAR=value cmd`
// prefix — "'VAR' is not recognized as an internal or external command".
// Measured on the 2026-09-06 release full sweep (SessionPublisherTrust,
// NotebookTour, BuildToolCommand, ...). Every shell-spawning test goes
// through these so the same source runs under bash and cmd.exe.
#if defined(_WIN32)
#  define CAJETA_PORTABLE_DEVNULL "NUL"
#  define CAJETA_PORTABLE_CD "cd /d "
#else
#  define CAJETA_PORTABLE_DEVNULL "/dev/null"
#  define CAJETA_PORTABLE_CD "cd "
#endif

// Renders `A=1 B=2 ` (POSIX) or `set "A=1" && set "B=2" && ` (cmd.exe) — a
// prefix that scopes environment variables to the ONE spawned command
// without touching this process's environment.
static inline std::string cajeta_env_prefix(
        std::initializer_list<std::pair<const char*, std::string>> vars) {
    std::string out;
    for (const auto& kv : vars) {
#if defined(_WIN32)
        out += std::string("set \"") + kv.first + "=" + kv.second + "\" && ";
#else
        out += std::string(kv.first) + "=" + kv.second + " ";
#endif
    }
    return out;
}
