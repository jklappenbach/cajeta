// cajeta-profiler Unit 12 — the CUPTI loader and binding state (plan 12.1.f,
// 12.2.a, 12.2.f's detection half; spec §5.4.2, §10.5, §12.5).
//
// The Unit-8 test pattern on NVIDIA: the ABSENT paths run everywhere —
// including this AMD development machine, which is where they matter most —
// and the present-path halves skip with a reason where no CUPTI exists,
// running for real on the profiler-tests lanes (PHOENIX and phoenix-wsl).
// The Activity machinery and the capability-ladder claims are NOT here: the
// ladder waits on Unit 1's §5.4.4 verdict; this loader does not.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "../../runtime/native/cajeta_prof_abi.h"
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#if defined(_WIN32)
static inline int setenv(const char* k, const char* v, int) { return _putenv_s(k, v); }
static inline int unsetenv(const char* k) { return _putenv_s(k, ""); }
#endif
using cajeta_test::CajetaJit;

namespace {

struct Cupti {
    std::unique_ptr<CajetaJit> jit;
    int32_t (*init)(void) = nullptr;
    void    (*reset)(void) = nullptr;
    int32_t (*state)(void) = nullptr;
    const char* (*reason)(void) = nullptr;
    const char* (*libPath)(void) = nullptr;
    int32_t (*entryCount)(void) = nullptr;
    int32_t (*entriesBound)(void) = nullptr;
    int32_t (*hasTsCallback)(void) = nullptr;
    int32_t (*versionIsWsl)(const char*) = nullptr;
    int32_t (*onWsl)(void) = nullptr;
};

Cupti& cu() {
    static Cupti c = [] {
        Cupti x;
        x.jit = CajetaJit::compile(
            "package test;\npublic final class Cu {\n"
            "    public static int32 run() { return 1; }\n}\n", "test.Cu");
        auto sym = [&](const char* n) { return x.jit->lookupRawSymbol(n); };
        x.init = reinterpret_cast<decltype(x.init)>(sym("__cajeta_prof_cupti_init"));
        x.reset = reinterpret_cast<decltype(x.reset)>(sym("__cajeta_prof_cupti_reset"));
        x.state = reinterpret_cast<decltype(x.state)>(sym("__cajeta_prof_cupti_state"));
        x.reason = reinterpret_cast<decltype(x.reason)>(sym("__cajeta_prof_cupti_reason"));
        x.libPath = reinterpret_cast<decltype(x.libPath)>(sym("__cajeta_prof_cupti_lib_path"));
        x.entryCount = reinterpret_cast<decltype(x.entryCount)>(sym("__cajeta_prof_cupti_entry_count"));
        x.entriesBound = reinterpret_cast<decltype(x.entriesBound)>(sym("__cajeta_prof_cupti_entries_bound"));
        x.hasTsCallback = reinterpret_cast<decltype(x.hasTsCallback)>(sym("__cajeta_prof_cupti_has_timestamp_callback"));
        x.versionIsWsl = reinterpret_cast<decltype(x.versionIsWsl)>(sym("__cajeta_prof_cupti_version_is_wsl"));
        x.onWsl = reinterpret_cast<decltype(x.onWsl)>(sym("__cajeta_prof_cupti_on_wsl"));
        return x;
    }();
    return c;
}

} // namespace

TEST(ProfilerCupti, entryPointsResolve) {
    auto& c = cu();
    ASSERT_NE(c.init, nullptr)          << "__cajeta_prof_cupti_init unresolved";
    ASSERT_NE(c.reset, nullptr);
    ASSERT_NE(c.state, nullptr);
    ASSERT_NE(c.reason, nullptr);
    ASSERT_NE(c.versionIsWsl, nullptr)  << "__cajeta_prof_cupti_version_is_wsl unresolved";
    ASSERT_NE(c.onWsl, nullptr);
}

// --- §5.4.2: an override is honored and NOT fallen back from ---------------

TEST(ProfilerCupti, absentOverrideIsReportedWithThePathTried) {
    auto& c = cu();
    ASSERT_TRUE(c.init && c.reset && c.reason);

    ::setenv("CAJETA_CUPTI_LIB", "/nonexistent/libcupti.so", 1);
    c.reset();
    EXPECT_EQ(c.init(), 0)
        << "an explicit override pointing nowhere must refuse, even on a "
           "machine that HAS CUPTI somewhere else — a typo must look like a "
           "typo, not like a working install";
    ::unsetenv("CAJETA_CUPTI_LIB");

    EXPECT_EQ(c.state(), CAJETA_CUPTI_ABSENT);
    const std::string why = c.reason() ? c.reason() : "";
    EXPECT_NE(why.find("/nonexistent/"), std::string::npos)
        << "the report does not name the library it tried; got [" << why << "]";
    EXPECT_NE(why.find("host submit-to-complete"), std::string::npos)
        << "the report does not say what the run degrades to; got [" << why << "]";

    c.reset();
    EXPECT_EQ(c.state(), CAJETA_CUPTI_UNATTEMPTED);
}

// --- the present path, where a CUPTI actually exists -----------------------

TEST(ProfilerCupti, presentCuptiBindsAllCoreEntryPoints) {
    auto& c = cu();
    ASSERT_TRUE(c.init && c.reset);
    c.reset();
    const int32_t ready = c.init();
    if (!ready) {
        GTEST_SKIP() << "CUPTI not loadable here ("
                     << (c.reason() ? c.reason() : "no reason")
                     << ") — the absent path is covered above; this half runs "
                        "on the profiler-tests lanes";
    }
    EXPECT_EQ(c.state(), CAJETA_CUPTI_READY);
    EXPECT_EQ(c.entriesBound(), c.entryCount())
        << "READY with a partial bind — the all-or-nothing rule broke";
    // Published for the lanes: which CUPTI bound, and whether §6.2's
    // timestamp callback exists there (its absence selects §6.9's conversion
    // path — a finding, not a failure).
    std::printf(" RESULT u12_cupti_path=%s\n", c.libPath() ? c.libPath() : "");
    std::printf(" RESULT u12_cupti_entries=%d\n", c.entriesBound());
    std::printf(" RESULT u12_has_timestamp_callback=%d\n", c.hasTsCallback());
    c.reset();
}

// --- §10.5 / §12.5: WSL identification -------------------------------------

TEST(ProfilerCupti, wslIsRecognizedFromRealKernelStrings) {
    auto& c = cu();
    ASSERT_NE(c.versionIsWsl, nullptr);
    // The strings real kernels print, not invented shapes: WSL2 spells
    // "microsoft" lowercase mid-token, WSL1 capitalized it, and no non-WSL
    // kernel says it at all.
    EXPECT_EQ(c.versionIsWsl(
        "Linux version 5.15.167.4-microsoft-standard-WSL2 "
        "(root@f9c826d3017f) (gcc (GCC) 11.2.0)"), 1);
    EXPECT_EQ(c.versionIsWsl(
        "Linux version 4.4.0-19041-Microsoft (Microsoft@Microsoft.com)"), 1);
    EXPECT_EQ(c.versionIsWsl(
        "Linux version 6.8.0-45-generic (buildd@lcy02-amd64-115) "
        "(x86_64-linux-gnu-gcc-13)"), 0);
    EXPECT_EQ(c.versionIsWsl(""), 0);
    EXPECT_EQ(c.versionIsWsl(nullptr), 0);
}

TEST(ProfilerCupti, theLiveProbeAgreesWithTheParser) {
    auto& c = cu();
    ASSERT_TRUE(c.versionIsWsl && c.onWsl);
#if defined(_WIN32)
    EXPECT_EQ(c.onWsl(), 0) << "Windows proper is not WSL";
#else
    std::string ver;
    if (FILE* f = std::fopen("/proc/version", "rb")) {
        char buf[512];
        size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        buf[got] = '\0';
        ver = buf;
    }
    EXPECT_EQ(c.onWsl(), c.versionIsWsl(ver.c_str()))
        << "the live probe and the parser disagree about [" << ver << "]";
#endif
    // Published so the lanes show the split: 0 on PHOENIX and on this
    // machine, 1 on phoenix-wsl — which is where §12.5's hazard lives.
    std::printf(" RESULT u12_on_wsl=%d\n", c.onWsl());
}
