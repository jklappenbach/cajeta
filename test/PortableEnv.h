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

static inline int setenv(const char* name, const char* value, int /*overwrite*/) {
    return _putenv_s(name, value ? value : "");
}

static inline int unsetenv(const char* name) {
    return _putenv_s(name, "");
}
#endif
