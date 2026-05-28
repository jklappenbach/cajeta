//
// Portable death-test matchers.
//
// gtest's ::testing::KilledBySignal is POSIX-only — Windows has no signals, so
// the header doesn't declare it and any reference fails to compile. Map the two
// abnormal-termination shapes the cajeta runtime produces onto whatever the
// host gtest understands:
//
//   abort()                 — bounds checks, unrecoverable throws
//   llvm.trap (ud2 / SIGILL)— overflow/UB traps from -ftrap-on-* options
//
// On Windows abort() exits the process with code 3, and an illegal instruction
// is reported as STATUS_ILLEGAL_INSTRUCTION (0xC000001D).
//

#pragma once

#include "gtest/gtest.h"
#include <csignal>

#ifdef _WIN32
#define CAJETA_DIED_BY_ABORT ::testing::ExitedWithCode(3)
#define CAJETA_DIED_BY_TRAP  ::testing::ExitedWithCode(static_cast<int>(0xC000001D))
#else
#define CAJETA_DIED_BY_ABORT ::testing::KilledBySignal(SIGABRT)
#define CAJETA_DIED_BY_TRAP  ::testing::KilledBySignal(SIGILL)
#endif
