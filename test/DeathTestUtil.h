//
// Portable death-test matchers.
//
// The cajeta runtime terminates abnormally in two distinct ways, and each maps
// to a different host-observable status depending on OS *and* CPU:
//
//   abort()        — bounds checks, unrecoverable throws
//     POSIX: SIGABRT          Windows: process exit code 3
//
//   llvm.trap      — overflow / UB traps from -ftrap-on-* compiler options
//     x86  : `ud2`  -> SIGILL
//     ARM64: `brk`  -> SIGTRAP
//     Windows: unhandled structured exception -> nonzero process exit code
//
// gtest's ::testing::KilledBySignal is POSIX-only and takes a single signal, so
// it can express neither the x86/ARM trap-signal split nor Windows. Define
// predicates that accept the full set instead.
//

#pragma once

#include "gtest/gtest.h"
#include <csignal>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifdef _WIN32

// abort() -> exit code 3 (CRT). Trap -> unhandled SEH exception surfaces as a
// nonzero, OS-assigned exit code we don't enumerate precisely.
struct CajetaDiedByTrapPred {
    bool operator()(int exit_status) const { return exit_status != 0; }
};
#define CAJETA_DIED_BY_ABORT ::testing::ExitedWithCode(3)
#define CAJETA_DIED_BY_TRAP  CajetaDiedByTrapPred()

#else

// gtest hands the raw waitpid() status to the predicate.
struct CajetaDiedByTrapPred {
    bool operator()(int wait_status) const {
        return WIFSIGNALED(wait_status) &&
               (WTERMSIG(wait_status) == SIGILL || WTERMSIG(wait_status) == SIGTRAP);
    }
};
#define CAJETA_DIED_BY_ABORT ::testing::KilledBySignal(SIGABRT)
#define CAJETA_DIED_BY_TRAP  CajetaDiedByTrapPred()

#endif
