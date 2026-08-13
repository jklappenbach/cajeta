// CAJETA_FORK_PER_TEST=1 — the fork-per-test prime server (compile-cache
// plan Unit 3F; spec §5.3).
//
// The stdlib prime is 77% FRONT-END (parse/typecheck into a live C++ object
// graph — measured 29.3s of 37.8s, 2026-08-10), which no on-disk artifact can
// cache. fork() can: the parent primes ONCE through the ordinary reuse path,
// then forks a copy-on-write child per matching test. Each child inherits the
// whole primed compiler state for free, runs exactly one test, and exits —
// per-test marginal cost falls to the test's own user compile, and a crashing
// test kills its child, never the server (per-test isolation that in-process
// chunk reuse cannot give).
//
// Coverage builds get clean attribution as a bonus: the parent __gcov_reset()s
// after priming, so a child's counter dump (CAJETA_FORK_GCOV_DIR=<base> →
// GCOV_PREFIX=<base>/<Suite.test>) holds ONLY that test's execution — the
// shared prime is excluded from every tree instead of smearing all of them.
//
// POSIX-only; Windows keeps the ordinary path (main.cpp never dispatches here).

#if !defined(_WIN32)

#include "gtest/gtest.h"
#include "jit/JitTestHelper.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dlfcn.h>

// Coverage hooks, resolved at RUNTIME. The previous ELF-style weak
// references (`__attribute__((weak))` declarations) linked fine on Linux
// but Mach-O's ld errors on a link-time-undefined weak function — the
// v0.19.0 aarch64-apple-darwin leg died on exactly that. dlsym against
// the process (the test binary links -rdynamic for the JIT on every
// platform) finds the symbols in instrumented builds and yields null
// otherwise — the same call-if-present contract, portably.
namespace {
void (*gcovResetFn())(void) {
    static void (*fn)(void) = reinterpret_cast<void (*)(void)>(
        dlsym(RTLD_DEFAULT, "__gcov_reset"));
    return fn;
}
void (*gcovDumpFn())(void) {
    static void (*fn)(void) = reinterpret_cast<void (*)(void)>(
        dlsym(RTLD_DEFAULT, "__gcov_dump"));
    return fn;
}
} // namespace

namespace {

// gtest-style glob: '*' any run, '?' any one char. Iterative with
// backtracking; no brackets/braces (gtest has none either).
bool globMatch(const char* pat, const char* str) {
    const char* star = nullptr;
    const char* resume = nullptr;
    while (*str) {
        if (*pat == '*') {
            star = pat++;
            resume = str;
        } else if (*pat == '?' || *pat == *str) {
            ++pat;
            ++str;
        } else if (star) {
            pat = star + 1;
            str = ++resume;
        } else {
            return false;
        }
    }
    while (*pat == '*') ++pat;
    return *pat == '\0';
}

// The gtest filter grammar: positive patterns, then an optional '-' and
// negative patterns, each side ':'-separated. Empty positive side = '*'.
bool sideMatches(const std::string& side, const std::string& name) {
    size_t start = 0;
    while (start <= side.size()) {
        size_t colon = side.find(':', start);
        std::string pat = side.substr(
            start, colon == std::string::npos ? std::string::npos
                                              : colon - start);
        if (!pat.empty() && globMatch(pat.c_str(), name.c_str())) return true;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return false;
}

bool filterAccepts(const std::string& filter, const std::string& name) {
    std::string positive = filter, negative;
    size_t dash = filter.find('-');
    if (dash != std::string::npos) {
        positive = filter.substr(0, dash);
        negative = filter.substr(dash + 1);
    }
    if (positive.empty()) positive = "*";
    if (!sideMatches(positive, name)) return false;
    if (!negative.empty() && sideMatches(negative, name)) return false;
    return true;
}

std::string safeName(std::string n) {
    for (char& c : n) {
        if (c == '/' || c == ':') c = '_';
    }
    return n;
}

}  // namespace

int cajetaForkPerTestMain() {
    // 1. Prime eagerly through the ordinary harness path (a trivial snippet
    // forces the full stdlib prime). Fork mode presupposes the in-process
    // reuse machinery; enable it if the caller didn't.
    setenv("CAJETA_STDLIB_REUSE", "1", /*overwrite=*/0);
    try {
        auto jit = cajeta_test::CajetaJit::compile(
            "package test;\n"
            "public final class ForkPrime {\n"
            "    public static int32 run() { return 0; }\n"
            "}\n",
            "test.ForkPrime");
        (void) jit;
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "cajeta: fork-per-test prime FAILED (%s); "
            "falling back to the in-process run\n", e.what());
        return RUN_ALL_TESTS();
    }

    // 2. Attribution reset: children inherit counters by COW, so zeroing here
    // keeps the shared prime out of every child's dump. No-op when
    // uninstrumented.
    if (gcovResetFn()) gcovResetFn()();

    const char* gcovDir = std::getenv("CAJETA_FORK_GCOV_DIR");
    const std::string filter = ::testing::GTEST_FLAG(filter);
    const bool runDisabled = ::testing::GTEST_FLAG(also_run_disabled_tests);

    auto* ut = ::testing::UnitTest::GetInstance();
    int ran = 0, failed = 0;
    std::vector<std::string> failedNames;
    for (int i = 0; i < ut->total_test_suite_count(); ++i) {
        const auto* suite = ut->GetTestSuite(i);
        for (int j = 0; j < suite->total_test_count(); ++j) {
            const auto* info = suite->GetTestInfo(j);
            std::string full =
                std::string(suite->name()) + "." + info->name();
            if (!filterAccepts(filter, full)) continue;
            if (!runDisabled
                    && (std::strncmp(info->name(), "DISABLED_", 9) == 0
                        || std::strncmp(suite->name(), "DISABLED_", 9) == 0)) {
                continue;
            }
            std::fflush(nullptr);
            pid_t pid = fork();
            if (pid < 0) {
                std::perror("cajeta: fork");
                return 2;
            }
            if (pid == 0) {
                // Child: exactly one test against the inherited primed state.
                ::testing::GTEST_FLAG(filter) = full;
                if (gcovDir) {
                    std::string prefix =
                        std::string(gcovDir) + "/" + safeName(full);
                    setenv("GCOV_PREFIX", prefix.c_str(), 1);
                }
                const int rc = RUN_ALL_TESTS();
                std::fflush(nullptr);
                if (gcovDumpFn()) gcovDumpFn()();
                _exit(rc != 0 ? 1 : 0);
            }
            ++ran;
            int status = 0;
            waitpid(pid, &status, 0);
            if (WIFSIGNALED(status)) {
                ++failed;
                failedNames.push_back(full);
                // The child died before gtest could report — print the
                // gtest-shaped line the sweep/coverage parsers key on.
                std::printf("[  FAILED  ] %s (killed by signal %d)\n",
                    full.c_str(), WTERMSIG(status));
            } else if (WEXITSTATUS(status) != 0) {
                ++failed;
                failedNames.push_back(full);
                // The child's own gtest output already carried the detail.
            }
        }
    }

    std::printf("[==========] fork-per-test: %d tests ran.\n", ran);
    std::printf("[  PASSED  ] %d tests.\n", ran - failed);
    if (failed) {
        std::printf("[  FAILED  ] %d tests, listed below:\n", failed);
        for (const auto& n : failedNames) {
            std::printf("[  FAILED  ] %s\n", n.c_str());
        }
    }
    std::fflush(nullptr);
    return failed ? 1 : 0;
}

#endif  // !_WIN32
