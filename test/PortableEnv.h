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
