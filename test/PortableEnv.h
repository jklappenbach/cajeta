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
#if defined(__APPLE__)
#include <mach-o/dyld.h>   // _NSGetExecutablePath
#elif defined(_WIN32)
// <process.h> already included above; GetModuleFileNameA needs windows.h.
#include <windows.h>
#endif

static inline std::filesystem::path cajeta_self_exe() {
#if defined(_WIN32)
    char buf[4096];
    DWORD n = ::GetModuleFileNameA(nullptr, buf, sizeof(buf));
    return std::filesystem::canonical(std::string(buf, n));
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t sz = sizeof(buf);
    if (::_NSGetExecutablePath(buf, &sz) != 0) return {};
    return std::filesystem::canonical(buf);
#else
    return std::filesystem::canonical("/proc/self/exe");
#endif
}
