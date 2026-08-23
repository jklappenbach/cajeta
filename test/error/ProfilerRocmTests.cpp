// cajeta-profiler Unit 8 — the ROCm backend (spec §5.2, §6.3).
//
// The unit's first requirement is not "device timing works". It is that a run
// SUCCEEDS with device timing degraded and reported when rocprofiler-sdk is not
// there (§5.2.2) — degradation is never fatal (§10.4), and a host
// submit-to-complete window always exists, so there is always something honest
// left to report.
//
// That half is asserted on every machine, ROCm or not, by pointing the loader
// at a library that cannot exist. A test that could only run with an AMD GPU
// attached would be checked by nobody, and this is precisely the path that
// matters on the machines that lack the hardware.
//
// The present-SDK half skips where the SDK is absent, and says so rather than
// passing quietly — "it worked" and "it never ran" must not look alike.
#include "gtest/gtest.h"

#include <cstdint>
#include <string>
#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "../../runtime/native/cajeta_prof_abi.h"

using cajeta_test::CajetaJit;

namespace {

// One JIT for the suite: the state under test lives in the JIT'd runtime copy,
// and a fresh compile per test would cost ~16s each for no isolation gain —
// every test resets first.
struct Rocm {
    std::unique_ptr<CajetaJit> jit;
    int32_t     (*init)(void)        = nullptr;
    void        (*reset)(void)       = nullptr;
    int32_t     (*state)(void)       = nullptr;
    const char* (*reason)(void)      = nullptr;
    const char* (*libPath)(void)     = nullptr;
    int32_t     (*tierFor)(int32_t)  = nullptr;
    const char* (*vtblName)(int32_t) = nullptr;
};

Rocm& roc() {
    static Rocm r = [] {
        Rocm x;
        x.jit = CajetaJit::compile(
            "package test;\n"
            "public final class R {\n"
            "    public static int32 run() { return 1; }\n"
            "}\n", "test.R");
        auto sym = [&](const char* n) { return x.jit->lookupRawSymbol(n); };
        x.init     = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_rocm_init"));
        x.reset    = reinterpret_cast<void (*)(void)>(sym("__cajeta_prof_rocm_reset"));
        x.state    = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_rocm_state"));
        x.reason   = reinterpret_cast<const char* (*)(void)>(sym("__cajeta_prof_rocm_reason"));
        x.libPath  = reinterpret_cast<const char* (*)(void)>(sym("__cajeta_prof_rocm_lib_path"));
        x.tierFor  = reinterpret_cast<int32_t (*)(int32_t)>(sym("__cajeta_prof_gpu_backend_tier"));
        x.vtblName = reinterpret_cast<const char* (*)(int32_t)>(sym("__cajeta_prof_gpu_backend_name"));
        return x;
    }();
    return r;
}

// Force the loader onto a path that cannot resolve, so "absent" is reproducible
// on a machine that HAS the SDK.
struct ForcedLib {
    std::string saved;
    bool had = false;
    explicit ForcedLib(const char* value) {
        if (const char* p = ::getenv("CAJETA_ROCPROF_LIB")) { saved = p; had = true; }
        if (value) ::setenv("CAJETA_ROCPROF_LIB", value, 1);
        else       ::unsetenv("CAJETA_ROCPROF_LIB");
    }
    ~ForcedLib() {
        if (had) ::setenv("CAJETA_ROCPROF_LIB", saved.c_str(), 1);
        else     ::unsetenv("CAJETA_ROCPROF_LIB");
    }
};

const int32_t kBackendHip = 1;   // caj_gpu_backend_name(): 0 cuda, 1 hip, 2 vulkan, 3 cpu

} // namespace

// ── 8.1.a — §5.2.2, absent SDK degrades and SAYS SO ──────────────────────

TEST(ProfilerRocm, absentSdkLeavesTheRunWorkingAtHostTier) {
    Rocm& r = roc();
    ASSERT_TRUE(r.init && r.reset && r.state && r.tierFor);

    ForcedLib forced("/nonexistent/librocprofiler-sdk.so.1");
    r.reset();
    const int32_t rc = r.init();

    EXPECT_EQ(rc, 0) << "init reported success with no library to bind";
    EXPECT_EQ(r.state(), CAJETA_ROCM_ABSENT);
    // The run is not broken — it degrades to the host window, which always
    // exists. TIER_HOST, not "no tier": a consumer weights by the mechanism,
    // and "we could not measure" is a different claim from "there was nothing".
    EXPECT_EQ(r.tierFor(kBackendHip), CAJETA_PROF_TIER_HOST)
        << "an absent rocprofiler-sdk must degrade to host submit-to-complete, "
           "not disable GPU timing outright (§5.2.2, §10.4)";
}

TEST(ProfilerRocm, absenceIsReportedWithTheReasonAndThePathTried) {
    Rocm& r = roc();
    ASSERT_TRUE(r.init && r.reset && r.reason && r.libPath);

    ForcedLib forced("/nonexistent/librocprofiler-sdk.so.1");
    r.reset();
    r.init();

    const std::string why = r.reason() ? r.reason() : "";
    EXPECT_FALSE(why.empty())
        << "degraded silently; §5.2.2 wants the degradation REPORTED, and a "
           "silent fallback is indistinguishable from working device timing";
    // The path is in the report because "not found" without saying where it
    // looked is the least actionable diagnostic there is.
    const std::string tried = r.libPath() ? r.libPath() : "";
    EXPECT_NE(tried.find("/nonexistent/"), std::string::npos)
        << "the report does not name the library it tried; got [" << tried << "]";
}

// The fallback must be the HOST backend, not a rocm vtable that silently
// returns zeros — a zero device span reads as a real measurement.
TEST(ProfilerRocm, absentSdkSelectsTheHostBackendNotAStubbedRocmOne) {
    Rocm& r = roc();
    ASSERT_TRUE(r.init && r.reset && r.vtblName);

    ForcedLib forced("/nonexistent/librocprofiler-sdk.so.1");
    r.reset();
    r.init();
    EXPECT_STREQ(r.vtblName(kBackendHip), "cpu")
        << "backend 1 bound a rocm vtable with nothing behind it";
}

// ── 8.1.b — with the SDK present, it actually binds ──────────────────────

TEST(ProfilerRocm, presentSdkBindsAndReportsReady) {
    Rocm& r = roc();
    ASSERT_TRUE(r.init && r.reset && r.state && r.reason);

    ForcedLib forced(nullptr);   // real search path
    r.reset();
    r.init();

    if (r.state() != CAJETA_ROCM_READY) {
        GTEST_SKIP() << "rocprofiler-sdk not loadable here (" << r.reason()
                     << ") — the absent path is covered above; this half needs "
                        "the SDK and says so rather than passing quietly";
    }
    EXPECT_STREQ(r.vtblName(kBackendHip), "rocm");
    const std::string path = r.libPath() ? r.libPath() : "";
    EXPECT_NE(path.find("librocprofiler-sdk"), std::string::npos)
        << "READY but no library path recorded; got [" << path << "]";
}

// reset() has to actually clear, or every test after the first inherits
// whichever library the first one happened to bind.
TEST(ProfilerRocm, resetReturnsToTheUnattemptedState) {
    Rocm& r = roc();
    ASSERT_TRUE(r.init && r.reset && r.state);

    { ForcedLib forced("/nonexistent/x.so"); r.reset(); r.init(); }
    EXPECT_EQ(r.state(), CAJETA_ROCM_ABSENT);
    r.reset();
    EXPECT_EQ(r.state(), CAJETA_ROCM_UNATTEMPTED)
        << "reset left the previous attempt's verdict in place";
}
