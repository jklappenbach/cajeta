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
#include <cstring>
#include <string>
#if !defined(_WIN32)
#  include <dlfcn.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <cerrno>
#endif
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
    int32_t     (*entryCount)(void)  = nullptr;
    int32_t     (*entriesBound)(void) = nullptr;
    int32_t     (*configure)(void)   = nullptr;
    int32_t     (*configured)(void)  = nullptr;
    int32_t     (*toolInitRan)(void) = nullptr;
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
        x.entryCount   = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_rocm_entry_count"));
        x.entriesBound = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_rocm_entries_bound"));
        x.configure    = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_rocm_configure"));
        x.configured   = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_rocm_configured"));
        x.toolInitRan  = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_rocm_tool_init_ran"));
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

// ── 8.2.a — entry-point binding ──────────────────────────────────────────
//
// dlopen succeeding is not the same as the SDK being usable. A library can load
// and still not export what buffered dispatch tracing needs — a partial ROCm
// install, a version older than the buffer-tracing API, or (as here) an
// unrelated library on the override path. If binding stopped at dlopen, that
// case would come out READY and every later call would go through a null
// pointer, which is a crash in the profiler rather than a degraded measurement.
//
// libm is the stand-in: it is present wherever glibc is, it loads, and it
// exports none of what is wanted. Building a fixture .so would test the same
// thing at more cost.

namespace {
const char* kLoadsButIsNotTheSdk = "libm.so.6";
}

TEST(ProfilerRocm, aLibraryThatLoadsWithoutTheEntryPointsIsAbsentNotReady) {
    Rocm& r = roc();
    ASSERT_TRUE(r.init && r.reset && r.state && r.entryCount && r.entriesBound);
    ASSERT_GT(r.entryCount(), 0) << "no entry points declared; the check is vacuous";

    ForcedLib forced(kLoadsButIsNotTheSdk);
    r.reset();
    const int32_t rc = r.init();

    EXPECT_EQ(rc, 0) << "a library with none of the entry points reported success";
    EXPECT_EQ(r.state(), CAJETA_ROCM_ABSENT)
        << "dlopen succeeded so binding declared victory; the entry points are "
           "what the backend actually calls";
    EXPECT_LT(r.entriesBound(), r.entryCount());
}

TEST(ProfilerRocm, aMissingEntryPointIsNamedInTheReason) {
    Rocm& r = roc();
    ASSERT_TRUE(r.init && r.reset && r.reason);

    ForcedLib forced(kLoadsButIsNotTheSdk);
    r.reset();
    r.init();

    const std::string why = r.reason() ? r.reason() : "";
    // "wrong version of the SDK" is only actionable if the report says which
    // symbol went missing — that names the version boundary that was crossed.
    EXPECT_NE(why.find("rocprofiler_"), std::string::npos)
        << "the reason does not name the entry point that failed to resolve; "
           "got [" << why << "]";
}

TEST(ProfilerRocm, everyEntryPointResolvesWhenTheRealSdkBinds) {
    Rocm& r = roc();
    ASSERT_TRUE(r.init && r.reset && r.state && r.entryCount && r.entriesBound);

    ForcedLib forced(nullptr);   // real search path
    r.reset();
    r.init();

    if (r.state() != CAJETA_ROCM_READY) {
        GTEST_SKIP() << "rocprofiler-sdk not loadable here (" << r.reason()
                     << ") — this half needs the SDK and says so rather than "
                        "passing quietly";
    }
    EXPECT_EQ(r.entriesBound(), r.entryCount())
        << "READY with " << r.entriesBound() << " of " << r.entryCount()
        << " entry points bound";
}

// ── 8.1.d / 8.2.b — the configuration window ─────────────────────────────
//
// rocprofiler intercepts HIP by installing itself while HIP loads. Once HIP has
// finished initializing that window is shut, force_configure refuses, and no
// dispatch is ever traced. §5.2.3 asks for that to be DETECTED, because the
// failure is otherwise silent: an unconfigured profiler still emits a GPU track,
// full of host-tier spans, which reads like a working device measurement.

TEST(ProfilerRocm, configuringWithoutABoundSdkDoesNotClaimSuccess) {
    Rocm& r = roc();
    ASSERT_TRUE(r.init && r.reset && r.state && r.configure);

    ForcedLib forced("/nonexistent/librocprofiler-sdk.so.1");
    r.reset();
    r.init();
    ASSERT_EQ(r.state(), CAJETA_ROCM_ABSENT);

    EXPECT_EQ(r.configure(), 0) << "configured a library that never bound";
    EXPECT_EQ(r.state(), CAJETA_ROCM_ABSENT)
        << "configure() overwrote the ABSENT verdict; nothing it can do "
           "improves on a library that is not there";
}

#if !defined(_WIN32)
// Late configuration, reproduced rather than simulated: the child configures
// rocprofiler itself — standing in for HIP or another ROCm tool getting there
// first — and only then asks the runtime to configure.
//
// It runs in a fork because configuring rocprofiler is process-wide and
// irreversible. Doing it in the test process would leave every later test
// looking at a configured SDK, and the state this test is about would be
// unreachable for the rest of the run.
//
// It is also declared BEFORE the success case below, which configures the test
// process for real. Order is declaration order under gtest; the child checks
// that assumption and reports kAlreadyConfigured rather than failing obscurely
// if it is ever violated.
namespace {
constexpr int kLateSeen          = 0;   // what the test is here to observe
constexpr int kNoSdk             = 2;   // nothing to test against; skip
constexpr int kAlreadyConfigured = 3;   // ordering broke — see the note above
constexpr int kWrongState        = 4;
constexpr int kWrongFallback     = 5;

using ConfigureFn = int32_t (*)(void*);
struct ToolResult { size_t size; void* initialize; void* finalize; void* tool_data; };
ToolResult g_childResult;

int childToolInit(void*, void*) { return 0; }

ToolResult* childConfigure(uint32_t, const char*, uint32_t, void*) {
    g_childResult.size       = sizeof(g_childResult);
    g_childResult.initialize = reinterpret_cast<void*>(&childToolInit);
    g_childResult.finalize   = nullptr;
    g_childResult.tool_data  = nullptr;
    return &g_childResult;
}
} // namespace

TEST(ProfilerRocm, configuringAfterSomethingElseAlreadyDidIsReportedAsLate) {
    Rocm& r = roc();
    ASSERT_TRUE(r.init && r.reset && r.state && r.configure && r.configured);

    // Learn readiness and the library path in the parent. init() binds but does
    // not configure, so asking does not spend the window this test needs.
    ForcedLib forced(nullptr);
    r.reset();
    r.init();
    if (r.state() != CAJETA_ROCM_READY) {
        GTEST_SKIP() << "rocprofiler-sdk not loadable here (" << r.reason()
                     << ") — the late-configuration window needs a real SDK to "
                        "be closed against";
    }
    const std::string libPath = r.libPath() ? r.libPath() : "";
    ASSERT_FALSE(libPath.empty());

    const pid_t pid = ::fork();
    ASSERT_NE(pid, -1) << "fork failed: " << ::strerror(errno);

    if (pid == 0) {
        if (r.configured()) ::_exit(kAlreadyConfigured);
        void* lib = ::dlopen(libPath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib) ::_exit(kNoSdk);
        auto force = reinterpret_cast<ConfigureFn>(::dlsym(lib, "rocprofiler_force_configure"));
        if (!force) ::_exit(kNoSdk);
        if (force(reinterpret_cast<void*>(&childConfigure)) != 0) ::_exit(kAlreadyConfigured);

        r.reset();
        r.init();
        if (r.configure() != 0)                  ::_exit(kWrongState);
        if (r.state() != CAJETA_ROCM_LATE)       ::_exit(kWrongState);
        // Detected is only half of it. A run whose profiler missed the window
        // still has a host submit-to-complete measurement, and must fall back
        // to it rather than to a rocm backend with no records behind it.
        if (r.tierFor(kBackendHip) != CAJETA_PROF_TIER_HOST) ::_exit(kWrongFallback);
        if (std::string(r.vtblName(kBackendHip)) != "cpu")   ::_exit(kWrongFallback);
        ::_exit(kLateSeen);
    }

    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status)) << "the child died rather than reporting";
    switch (WEXITSTATUS(status)) {
        case kLateSeen: break;
        case kNoSdk:
            GTEST_SKIP() << "the child could not load the SDK the parent bound";
            break;
        case kAlreadyConfigured:
            FAIL() << "rocprofiler was already configured in this process, so the "
                      "late window could not be closed on purpose — this test must "
                      "run before the one that configures for real";
            break;
        case kWrongState:
            FAIL() << "configuring after rocprofiler was already initialized was "
                      "accepted instead of reported as CAJETA_ROCM_LATE (§5.2.3)";
            break;
        case kWrongFallback:
            FAIL() << "a late configuration left GPU timing pointed at the rocm "
                      "backend, which has no records behind it (§5.2.4)";
            break;
        default:
            FAIL() << "child exited " << WEXITSTATUS(status);
    }
}
#endif  // !_WIN32

TEST(ProfilerRocm, configureRegistersTheToolWithTheSdk) {
    Rocm& r = roc();
    ASSERT_TRUE(r.init && r.reset && r.state && r.configure && r.configured && r.toolInitRan);

    ForcedLib forced(nullptr);
    r.reset();
    r.init();
    if (r.state() != CAJETA_ROCM_READY) {
        GTEST_SKIP() << "rocprofiler-sdk not loadable here (" << r.reason() << ")";
    }

    EXPECT_EQ(r.configure(), 1) << "configure failed: " << r.reason();
    EXPECT_EQ(r.state(), CAJETA_ROCM_READY);
    EXPECT_EQ(r.configured(), 1);
    // force_configure returning success is not the same as the SDK adopting us.
    // The callback running is what proves the registration took.
    EXPECT_EQ(r.toolInitRan(), 1)
        << "force_configure reported success but the SDK never called back";
}

TEST(ProfilerRocm, configuringAgainIsSuccessNotLateness) {
    Rocm& r = roc();
    ASSERT_TRUE(r.init && r.reset && r.state && r.configure && r.configured);

    ForcedLib forced(nullptr);
    r.reset();
    r.init();
    if (r.state() != CAJETA_ROCM_READY) {
        GTEST_SKIP() << "rocprofiler-sdk not loadable here (" << r.reason() << ")";
    }
    ASSERT_EQ(r.configure(), 1);

    // The second call finds rocprofiler initialized — by US. Reading that as
    // lateness would report a failure that did not happen, and would drop a
    // working device backend on the second trace of a run.
    EXPECT_EQ(r.configure(), 1) << "reported failure the second time: " << r.reason();
    EXPECT_EQ(r.state(), CAJETA_ROCM_READY);
}
