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
#include "../xpu/XpuDeviceTestUtil.h"
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <utility>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
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
    // Unit 12's record path (12.1.a-e, g).
    int32_t (*kindAllowed)(int32_t) = nullptr;
    int32_t (*prefixBytes)(void) = nullptr;
    int32_t (*decodeKernel)(const void*, int64_t, int64_t*, int64_t*, int32_t*) = nullptr;
    int32_t (*noteSubscribe)(int32_t) = nullptr;
    int32_t (*degraded)(void) = nullptr;
    int32_t (*tsFirst)(void) = nullptr;
    int32_t (*tsStatus)(void) = nullptr;
    int32_t (*tsRegistered)(void) = nullptr;
    int64_t (*records)(void) = nullptr;
    int64_t (*rejected)(void) = nullptr;
    // 12.2.c/d — correlation and the launch chokepoint.
    int32_t (*decodeExternal)(const void*, int64_t, int32_t*, int64_t*) = nullptr;
    void    (*corrReset)(void) = nullptr;
    int32_t (*corrNote)(int32_t, int64_t) = nullptr;
    int32_t (*corrLookup)(int32_t, int64_t*) = nullptr;
    int32_t (*corrCapacity)(void) = nullptr;
    int64_t (*corrDropped)(void) = nullptr;
    int32_t (*push)(int64_t) = nullptr;
    int32_t (*pop)(void) = nullptr;
    int32_t (*tracing)(void) = nullptr;
    int64_t (*pushes)(void) = nullptr;
    int64_t (*pops)(void) = nullptr;
    // 12.2.d — the seam side of the chokepoint.
    int32_t (*launch)(const char*, int32_t, int32_t, int32_t, int32_t, int32_t,
                      int32_t, int64_t, int64_t, int32_t, int32_t,
                      void (*)(void*), void*) = nullptr;
    const char* (*vtblName)(int32_t) = nullptr;
    int32_t (*tierFor)(int32_t) = nullptr;
    void    (*pendingReset)(void) = nullptr;
    int32_t (*gpuFlush)(void) = nullptr;
    int32_t (*enableKind)(int32_t) = nullptr;
    int32_t (*kindsEnabled)(void) = nullptr;
    int32_t (*sinkRegister)(int32_t (*)(const CajetaGpuEvent*, int32_t, void*),
                            void*, int32_t) = nullptr;
    int32_t (*sinkUnregister)(int32_t) = nullptr;
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
        x.kindAllowed = reinterpret_cast<decltype(x.kindAllowed)>(sym("__cajeta_prof_cupti_kind_is_allowed"));
        x.prefixBytes = reinterpret_cast<decltype(x.prefixBytes)>(sym("__cajeta_prof_cupti_kernel_prefix_bytes"));
        x.decodeKernel = reinterpret_cast<decltype(x.decodeKernel)>(sym("__cajeta_prof_cupti_decode_kernel"));
        x.noteSubscribe = reinterpret_cast<decltype(x.noteSubscribe)>(sym("__cajeta_prof_cupti_note_subscribe_result"));
        x.degraded = reinterpret_cast<decltype(x.degraded)>(sym("__cajeta_prof_cupti_degraded"));
        x.tsFirst = reinterpret_cast<decltype(x.tsFirst)>(sym("__cajeta_prof_cupti_ts_callback_registered_first"));
        x.tsStatus = reinterpret_cast<decltype(x.tsStatus)>(sym("__cajeta_prof_cupti_ts_status"));
        x.tsRegistered = reinterpret_cast<decltype(x.tsRegistered)>(sym("__cajeta_prof_cupti_ts_registered"));
        x.records = reinterpret_cast<decltype(x.records)>(sym("__cajeta_prof_cupti_records"));
        x.rejected = reinterpret_cast<decltype(x.rejected)>(sym("__cajeta_prof_cupti_rejected"));
        x.decodeExternal = reinterpret_cast<decltype(x.decodeExternal)>(sym("__cajeta_prof_cupti_decode_external"));
        x.corrReset = reinterpret_cast<decltype(x.corrReset)>(sym("__cajeta_prof_cupti_corr_reset"));
        x.corrNote = reinterpret_cast<decltype(x.corrNote)>(sym("__cajeta_prof_cupti_corr_note"));
        x.corrLookup = reinterpret_cast<decltype(x.corrLookup)>(sym("__cajeta_prof_cupti_corr_lookup"));
        x.corrCapacity = reinterpret_cast<decltype(x.corrCapacity)>(sym("__cajeta_prof_cupti_corr_capacity"));
        x.corrDropped = reinterpret_cast<decltype(x.corrDropped)>(sym("__cajeta_prof_cupti_corr_dropped"));
        x.push = reinterpret_cast<decltype(x.push)>(sym("__cajeta_prof_cupti_push"));
        x.pop = reinterpret_cast<decltype(x.pop)>(sym("__cajeta_prof_cupti_pop"));
        x.tracing = reinterpret_cast<decltype(x.tracing)>(sym("__cajeta_prof_cupti_tracing"));
        x.pushes = reinterpret_cast<decltype(x.pushes)>(sym("__cajeta_prof_cupti_pushes"));
        x.pops = reinterpret_cast<decltype(x.pops)>(sym("__cajeta_prof_cupti_pops"));
        x.launch = reinterpret_cast<decltype(x.launch)>(sym("__cajeta_prof_gpu_launch"));
        x.vtblName = reinterpret_cast<decltype(x.vtblName)>(sym("__cajeta_prof_gpu_backend_name"));
        x.tierFor = reinterpret_cast<decltype(x.tierFor)>(sym("__cajeta_prof_gpu_backend_tier"));
        x.pendingReset = reinterpret_cast<decltype(x.pendingReset)>(sym("__cajeta_prof_gpu_pending_reset"));
        x.gpuFlush = reinterpret_cast<decltype(x.gpuFlush)>(sym("__cajeta_prof_gpu_flush"));
        x.enableKind = reinterpret_cast<decltype(x.enableKind)>(sym("__cajeta_prof_cupti_enable_kind"));
        x.kindsEnabled = reinterpret_cast<decltype(x.kindsEnabled)>(sym("__cajeta_prof_cupti_kinds_enabled"));
        x.sinkRegister = reinterpret_cast<decltype(x.sinkRegister)>(sym("__cajeta_prof_gpu_sink_register"));
        x.sinkUnregister = reinterpret_cast<decltype(x.sinkUnregister)>(sym("__cajeta_prof_gpu_sink_unregister"));
        return x;
    }();
    return c;
}

void noKernel(void*) {}

int32_t dropSink(const CajetaGpuEvent*, int32_t, void*) { return 0; }

// 12.3.b's judgment, factored out so the measurement and the test of the rule
// are the SAME code. A second implementation of the rule in the test would
// agree with itself while the published verdict did whatever it liked.
struct LaneStats { double mean = 0; double spread = 0; };

LaneStats laneStats(std::vector<double> v) {
    LaneStats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    s.mean = sum / static_cast<double>(v.size());
    s.spread = v.back() - v.front();
    return s;
}

// A delta no larger than the two lanes' combined run-to-run spread has not
// been measured — it has been bounded. The audit probe published 957.5 ns
// against a 1320 ns spread and called it CUPTI's cost.
bool overheadIsResolvable(const LaneStats& armed, const LaneStats& base,
                          double* deltaOut, double* noiseOut) {
    const double delta = armed.mean - base.mean;
    const double noise = armed.spread + base.spread;
    if (deltaOut) *deltaOut = delta;
    if (noiseOut) *noiseOut = noise;
    return std::abs(delta) > noise;
}

// An armed seam. `__cajeta_prof_gpu_launch` returns before it touches a vtbl
// when no sink is live (7.1.e) — so a launch test without this measures
// nothing, which is how the first draft of these tests passed with the
// chokepoint deliberately wired into the WRONG lane.
struct ArmedSink {
    Cupti& c;
    int32_t id;
    explicit ArmedSink(Cupti& cu)
        : c(cu), id(cu.sinkRegister(dropSink, nullptr, CAJETA_GPU_SINK_BATCHED)) {}
    ~ArmedSink() { if (id >= 0) c.sinkUnregister(id); }
    bool ok() const { return id >= 0; }
};

// Arm the real CUPTI if this machine has one. Mirrors ProfilerRocm::armTracing.
bool armTracing(Cupti& c) {
    c.reset();
    c.init();
    if (c.state() != CAJETA_CUPTI_READY) return false;
    c.enableKind(10);   // CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL; the only kind
                        // this backend may enable (12.1.b)
    return c.tracing() != 0;
}

// Why a lane could not be armed, in the terms `tracing()` actually gates on.
//
// `reason()` alone is NOT that: a refused timestamp callback still leaves the
// state READY (records arrive in CUPTI's own domain and §6.9 converts), so on
// phoenix-wsl the skip printed a message about the timestamp callback while
// the thing that actually failed was the activity-kind enable. A skip that
// names the wrong cause is worse than one that names none — it is what a
// reader will believe.
std::string armFailure(Cupti& c) {
    std::string s = "CUPTI not armed here:";
    s += " state=" + std::to_string(c.state ? c.state() : -1);
    s += " kinds_enabled=" + std::to_string(c.kindsEnabled ? c.kindsEnabled() : -1);
    s += " degraded=" + std::to_string(c.degraded ? c.degraded() : -1);
    s += " ts_status=" + std::to_string(c.tsStatus ? c.tsStatus() : -1);
    s += " tracing=" + std::to_string(c.tracing ? c.tracing() : -1);
    s += "\n  reason(): ";
    s += (c.reason && c.reason()) ? c.reason() : "(none)";
    return s;
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

// --- the pinned-library rule ------------------------------------------------
// CUPTI patches libcuda's driver dispatch table the first time any CUPTI API
// runs — register_timestamp_callback does, on every bind, and on WSL2 it
// initializes the interception layer and THEN refuses (CUptiResult 39).
// dlclose'ing libcupti afterwards unmaps the code those patched pointers
// target while the pointers stay in the driver; the next cuInit calls through
// the stale hook and the process SIGSEGVs with rip == the dead address. That
// was the WSL SIGSEGV in XpuDeviceProfileNvidiaDeviceTests.rawQueryAnswersOnCuda
// whenever ANY CUPTI test ran earlier in the same binary (gdb: libcupti mapped
// at 0x7ffff50b6170-0x7ffff54c4804, unloaded, then cuInit -> rip
// 0x7ffff51032b0, inside the former range). The rule under test: once bound,
// libcupti is pinned for the life of the process — reset() clears state,
// never the mapping — and the pinned library is reusable by a later init.
//
// The sequence is exactly the one that faulted, and deliberately calls no
// cuInit before the bind: the crash was on the FIRST cuInit after a
// bind+reset. Pre-fix, the cudaAvailable() line below killed the process.
TEST(ProfilerCupti, aBoundCuptiStaysMappedAcrossResetSoCuInitDoesNotFault) {
    auto& c = cu();
    ASSERT_TRUE(c.init && c.reset && c.state);
    c.reset();
    if (!c.init()) {
        GTEST_SKIP() << "CUPTI not loadable here ("
                     << (c.reason() ? c.reason() : "no reason")
                     << ") — nothing to pin";
    }
    ASSERT_EQ(c.state(), CAJETA_CUPTI_READY);
    // bind (a real CUPTI call ran, hooks are in the driver) -> reset -> cuInit.
    c.reset();
    EXPECT_EQ(c.state(), CAJETA_CUPTI_UNATTEMPTED);
    const bool cuda = cajeta::xpu::test::cudaAvailable();   // pre-fix: SIGSEGV
    if (!cuda) {
        GTEST_SKIP() << "no CUDA device: the post-reset cuInit ran WITHOUT "
                        "faulting, which is the contract; the reuse half below "
                        "needs a device";
    }
    // And the pinned library is reusable: a second bind is still READY.
    EXPECT_TRUE(c.init())
        << "a later init could not rebind the pinned libcupti";
    EXPECT_EQ(c.state(), CAJETA_CUPTI_READY);
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

// ── Unit 12's record path ────────────────────────────────────────────────
//
// MEASURED, not recalled. Compiled against a real cupti_activity.h on
// 2026-08-30, offsetof() across every kernel record version it declares:
//
//   version   kind  start  end  correlationId  sizeof
//   Kernel2      0      8   16             84     112   <-- DIFFERENT
//   Kernel3      0     16   24             92     120
//   Kernel4      0     16   24             92     144
//   Kernel5      0     16   24             92     160
//   Kernel6      0     16   24             92     168
//   Kernel7      0     16   24             92     176
//   Kernel8      0     16   24             92     200
//   Kernel9      0     16   24             92     208
//
// So the struct GROWS at the tail while the prefix through correlationId
// stays put from Kernel3 on, which is what spec §5.4.5 means by reading only
// stable prefix fields, and why the backend must never cast a record to a
// version-specific struct. Kernel2 (pre-CUDA 9) is the floor and would
// misparse; the plausibility checks below are what stop a misparse becoming a
// published measurement rather than a crash.
namespace {

constexpr int32_t kKindKernel           = 3;    // CUPTI_ACTIVITY_KIND_KERNEL
constexpr int32_t kKindConcurrentKernel  = 10;   // ..._CONCURRENT_KERNEL
constexpr size_t  kOffKind = 0, kOffStart = 16, kOffEnd = 24, kOffCorr = 92;

// A record built the way CUPTI lays one out, sized like the largest version
// so a decoder that reads past its prefix has somewhere to go wrong.
struct FakeRecord {
    unsigned char bytes[208];
    FakeRecord(int32_t kind, uint64_t start, uint64_t end, uint32_t corr) {
        std::memset(bytes, 0xAB, sizeof bytes);   // poison: nothing may read tail
        std::memcpy(bytes + kOffKind,  &kind,  sizeof kind);
        std::memcpy(bytes + kOffStart, &start, sizeof start);
        std::memcpy(bytes + kOffEnd,   &end,   sizeof end);
        std::memcpy(bytes + kOffCorr,  &corr,  sizeof corr);
    }
};

} // namespace

// 12.1.b — CUPTI_ACTIVITY_KIND_KERNEL is never enabled. It SERIALIZES kernel
// execution; CONCURRENT_KERNEL is the one that measures what actually ran.
// Enabling the wrong one changes the program being measured.
TEST(ProfilerCupti, theSerializingKernelKindIsNeverAllowed) {
    ASSERT_NE(cu().kindAllowed, nullptr);
    EXPECT_EQ(cu().kindAllowed(kKindConcurrentKernel), 1)
        << "CONCURRENT_KERNEL is the kind this backend exists to read";
    EXPECT_EQ(cu().kindAllowed(kKindKernel), 0)
        << "KIND_KERNEL serializes execution - enabling it would change the "
           "program being measured, not just observe it";
}

// 12.1.a — a well-formed record decodes, and its timestamps are non-zero.
TEST(ProfilerCupti, aConcurrentKernelRecordDecodesWithNonZeroTimestamps) {
    ASSERT_NE(cu().decodeKernel, nullptr);
    const FakeRecord r(kKindConcurrentKernel, 1000, 4000, 77);
    int64_t start = -1, end = -1; int32_t corr = -1;
    EXPECT_EQ(cu().decodeKernel(r.bytes, sizeof r.bytes, &start, &end, &corr), 1);
    EXPECT_EQ(start, 1000);
    EXPECT_EQ(end, 4000);
    EXPECT_EQ(corr, 77);
    EXPECT_GT(start, 0);
    EXPECT_GT(end, 0);
}

// 12.1.g — only the stable prefix is read. A record truncated one byte before
// the end of correlationId must be refused, not read past.
TEST(ProfilerCupti, aRecordShorterThanTheStablePrefixIsRefused) {
    ASSERT_NE(cu().prefixBytes, nullptr);
    EXPECT_EQ(cu().prefixBytes(), 96)
        << "the prefix runs through correlationId at offset 92 (+4)";
    const FakeRecord r(kKindConcurrentKernel, 1000, 4000, 77);
    int64_t start = -1, end = -1; int32_t corr = -1;
    EXPECT_EQ(cu().decodeKernel(r.bytes, 95, &start, &end, &corr), 0)
        << "95 bytes cannot contain correlationId; decoding it would read "
           "whatever follows the record";
    EXPECT_EQ(cu().decodeKernel(r.bytes, 96, &start, &end, &corr), 1)
        << "96 bytes is exactly the prefix and must be enough";
}

// 12.1.g — a kind this backend did not ask for is skipped, not decoded.
TEST(ProfilerCupti, aNonKernelRecordIsSkippedRatherThanDecoded) {
    const FakeRecord r(kKindKernel, 1000, 4000, 77);   // the serializing kind
    int64_t start = -1, end = -1; int32_t corr = -1;
    EXPECT_EQ(cu().decodeKernel(r.bytes, sizeof r.bytes, &start, &end, &corr), 0);
    EXPECT_EQ(start, -1) << "outputs must be untouched when nothing was decoded";
}

// 12.1.d — the known CUPTI regression: a kernel record whose timestamps are
// zero. Zero is not a time; publishing it would put a span at the epoch and
// make every later duration nonsense.
TEST(ProfilerCupti, aZeroKernelTimestampIsRejectedAndCounted) {
    ASSERT_NE(cu().rejected, nullptr);
    const int64_t before = cu().rejected();
    int64_t start = -1, end = -1; int32_t corr = -1;

    const FakeRecord zeroStart(kKindConcurrentKernel, 0, 4000, 77);
    EXPECT_EQ(cu().decodeKernel(zeroStart.bytes, sizeof zeroStart.bytes,
                                &start, &end, &corr), -1);
    const FakeRecord zeroEnd(kKindConcurrentKernel, 1000, 0, 77);
    EXPECT_EQ(cu().decodeKernel(zeroEnd.bytes, sizeof zeroEnd.bytes,
                                &start, &end, &corr), -1);
    const FakeRecord bothZero(kKindConcurrentKernel, 0, 0, 77);
    EXPECT_EQ(cu().decodeKernel(bothZero.bytes, sizeof bothZero.bytes,
                                &start, &end, &corr), -1);

    EXPECT_EQ(cu().rejected() - before, 3)
        << "each rejection must be COUNTED - a silently dropped record is how "
           "a backend reports a clean run while measuring nothing";
}

// 12.1.d — end before start is the other way a record lies. This is also the
// shape a Kernel2 misparse takes, which is why it is rejected rather than
// clamped: a clamp would publish the misparse as a plausible span.
TEST(ProfilerCupti, anInvertedKernelSpanIsRejected) {
    const int64_t before = cu().rejected();
    const FakeRecord r(kKindConcurrentKernel, 4000, 1000, 77);
    int64_t start = -1, end = -1; int32_t corr = -1;
    EXPECT_EQ(cu().decodeKernel(r.bytes, sizeof r.bytes, &start, &end, &corr), -1);
    EXPECT_EQ(cu().rejected() - before, 1);
}

// 12.1.e — a second CUPTI subscriber in the process. CUPTI allows exactly one;
// the second gets CUPTI_ERROR_MULTIPLE_SUBSCRIBERS_NOT_SUPPORTED. Aborting
// there would take down a program that merely ran under Nsight, so the
// backend degrades to a no-op and says so.
TEST(ProfilerCupti, aSecondSubscriberDegradesInsteadOfAborting) {
    ASSERT_NE(cu().noteSubscribe, nullptr);
    cu().reset();
    EXPECT_EQ(cu().degraded(), 0) << "a fresh backend is not degraded";

    constexpr int32_t kMultipleSubscribers = 39;  // CUPTI_ERROR_MULTIPLE_SUBSCRIBERS_NOT_SUPPORTED
    EXPECT_EQ(cu().noteSubscribe(kMultipleSubscribers), 0)
        << "the call reports failure to attach";
    EXPECT_EQ(cu().degraded(), 1) << "and the backend is now a no-op";

    const std::string why = cu().reason();
    EXPECT_NE(why.find("subscriber"), std::string::npos)
        << "the state must carry an actionable sentence, got: " << why;

    cu().reset();
    EXPECT_EQ(cu().degraded(), 0);
    EXPECT_EQ(cu().noteSubscribe(0), 1) << "CUPTI_SUCCESS attaches normally";
    EXPECT_EQ(cu().degraded(), 0);
    cu().reset();
}

// 12.1.c — spec §6.2: records must arrive already in the host clock domain,
// which requires the timestamp callback to be registered BEFORE any activity
// kind is enabled.
//
// The property is ORDERING, and the first version of this test did not say so:
// it asserted that a READY backend with the symbol present must have
// registered. phoenix-wsl proved that wrong on 2026-08-30 (run 33328180931) by
// resolving the symbol and then REFUSING the registration, and the test
// reported an ordering failure for what is a platform difference. Registration
// can fail; what may never happen is registering after an enable, or failing
// silently.
TEST(ProfilerCupti, theTimestampCallbackIsRegisteredBeforeAnyKindIsEnabled) {
    ASSERT_NE(cu().tsFirst, nullptr);
    ASSERT_NE(cu().tsStatus, nullptr);
    cu().reset();
    EXPECT_EQ(cu().tsFirst(), 0) << "nothing registered yet on a fresh backend";
    EXPECT_EQ(cu().tsStatus(), -1) << "and no attempt has been made";
    cu().init();

    if (cu().state() != CAJETA_CUPTI_READY) {
        GTEST_SKIP() << "no CUPTI here; " << cu().reason();
    }

    const std::string why = cu().reason();
    if (!cu().hasTsCallback()) {
        EXPECT_EQ(cu().tsStatus(), -1) << "absent symbol means no attempt";
        EXPECT_NE(why.find("conversion path"), std::string::npos)
            << "an absent callback must say the conversion path applies: " << why;
    } else if (cu().tsRegistered()) {
        EXPECT_EQ(cu().tsStatus(), 0);
        EXPECT_EQ(cu().tsFirst(), 1)
            << "THE ordering property: registration landed while zero activity "
               "kinds were enabled";
    } else {
        // Registration was refused. That is allowed, and it is what WSL2 does.
        // What is NOT allowed is for it to be silent: the operator is now on
        // the conversion path and nothing else would tell them.
        EXPECT_GT(cu().tsStatus(), 0) << "a refusal must carry its CUptiResult";
        EXPECT_EQ(cu().tsFirst(), 0) << "a refused registration is not a registration";
        EXPECT_NE(why.find("REFUSED"), std::string::npos)
            << "a refused callback must SAY so - otherwise the backend is "
               "silently in a different clock domain: " << why;
        EXPECT_NE(why.find("6.9"), std::string::npos)
            << "and must name the path that now applies: " << why;
    }
    cu().reset();
}

// ── 12.2.d, the SEAM half ────────────────────────────────────────────────
//
// The chokepoint had no caller. `__cajeta_prof_cupti_push`/`pop` were written,
// unit-tested directly, and wired to nothing: `caj_gpu_vtbl_for` had no CUDA
// arm at all, so a CAJ_GPU_BACKEND_CUDA launch resolved to the CPU vtbl and
// the correlation stack was never touched by a launch. The item was ticked on
// the strength of tests that called the two functions themselves — which is
// exactly the shape of check that cannot fail. These four ask the SEAM.

// Runs everywhere, including this AMD box: with no CUPTI the CUDA id must
// degrade to the host lane (§5.1.4) rather than to nothing.
TEST(ProfilerCupti, aCudaLaunchWithoutCuptiFallsBackToTheHostLane) {
    Cupti& c = cu();
    ASSERT_TRUE(c.vtblName && c.tierFor && c.reset && c.init);
    c.reset();
    c.init();
    if (c.tracing()) GTEST_SKIP() << "CUPTI is armed here; this is the absent path";

    EXPECT_STREQ(c.vtblName(CAJ_GPU_BACKEND_CUDA), "cpu");
    EXPECT_EQ(c.tierFor(CAJ_GPU_BACKEND_CUDA), CAJETA_PROF_TIER_HOST);
}

// The counterpart claim, stated so it can be wrong: on a machine with no CUDA
// the seam must not be pushing correlation ids into a stack nothing reads.
TEST(ProfilerCupti, theCorrelationStackIsUntouchedWhenTheBackendNeverBound) {
    Cupti& c = cu();
    ASSERT_TRUE(c.launch && c.pushes && c.pops && c.pendingReset);
    c.reset();
    c.init();
    if (c.tracing()) GTEST_SKIP() << "CUPTI is armed here; this is the absent path";

    ArmedSink armed(c);
    ASSERT_TRUE(armed.ok());
    const int64_t pushes0 = c.pushes(), pops0 = c.pops();
    for (int i = 0; i < 32; ++i)
        c.launch("k", 1, 1, 1, 64, 1, 1, 0, 0, 0, CAJ_GPU_BACKEND_CUDA,
                 noKernel, nullptr);
    c.pendingReset();
    c.gpuFlush();

    EXPECT_EQ(c.pushes() - pushes0, 0)
        << "the host lane pushed a correlation id no CUPTI will ever read";
    EXPECT_EQ(c.pops() - pops0, 0);
}

// The present path: skips off-hardware, runs for real on PHOENIX/phoenix-wsl.
TEST(ProfilerCupti, aCudaLaunchSelectsTheCuptiBackendWhenItIsReady) {
    Cupti& c = cu();
    ASSERT_TRUE(c.vtblName && c.tierFor);
    if (!armTracing(c)) GTEST_SKIP() << armFailure(c);

    EXPECT_STREQ(c.vtblName(CAJ_GPU_BACKEND_CUDA), "cuda");
}

// The claim 12.2.d actually makes, asked of the seam rather than of the two
// functions: N launches produce N pushes and N pops, paired.
TEST(ProfilerCupti, everyCudaLaunchPushesAndPopsItsCorrelationId) {
    Cupti& c = cu();
    ASSERT_TRUE(c.launch && c.pushes && c.pops && c.pendingReset);
    if (!armTracing(c)) GTEST_SKIP() << armFailure(c);

    ArmedSink armed(c);
    ASSERT_TRUE(armed.ok());
    const int64_t pushes0 = c.pushes(), pops0 = c.pops();
    const int kLaunches = 64;
    for (int i = 0; i < kLaunches; ++i)
        c.launch("k", 1, 1, 1, 64, 1, 1, 0, 0, 0, CAJ_GPU_BACKEND_CUDA,
                 noKernel, nullptr);
    c.pendingReset();
    c.gpuFlush();

    EXPECT_EQ(c.pushes() - pushes0, kLaunches);
    EXPECT_EQ(c.pops() - pops0, kLaunches)
        << "an unpaired push leaves the correlation stack deeper every launch";
}

// ── 12.3.b — per-launch overhead, measured against the code path that ships ──
//
// The audit probe's numbers (run 33324968136, RTX 4090) do not answer this: it
// measured its OWN CUPTI usage, and the backend adds an external-correlation
// push/pop per launch that the probe never makes. It also published
// +957.5 ns/launch as CUPTI's cost when the run-to-run spread at 7 reps was
// 1320 ns — a number smaller than its own noise, reported as a measurement.
//
// So this publishes the SPREAD alongside the mean and refuses to call a delta
// a cost unless it clears both lanes' spreads. And the arms alternate: a fixed
// order lets a decaying background load masquerade as a difference between
// them.
TEST(ProfilerCupti, perLaunchOverheadIsMeasuredAndPublished) {
    Cupti& c = cu();
    ASSERT_TRUE(c.launch && c.pendingReset && c.gpuFlush && c.sinkRegister);
    if (!armTracing(c)) GTEST_SKIP() << armFailure(c);
    ASSERT_STREQ(c.vtblName(CAJ_GPU_BACKEND_CUDA), "cuda")
        << "measuring the CPU lane and calling it the backend's cost is how "
           "the audit probe's numbers got here in the first place";

    auto nowNs = reinterpret_cast<int64_t (*)(void)>(
        cu().jit->lookupRawSymbol("__cajeta_currentTimeNanos"));
    ASSERT_NE(nowNs, nullptr);

    ArmedSink armed(c);
    ASSERT_TRUE(armed.ok());

    // Reps, not batch size, is where the samples come from. `caj_gpu_park`
    // linear-scans a 256-slot table for a free entry, and only the CUDA lane
    // parks — so a big batch makes late launches pay a scan proportional to
    // how many earlier ones are still parked, and the published "overhead"
    // becomes a fact about the batch size rather than about the backend. In
    // production the table drains continuously as records arrive, so a shallow
    // batch is the honest shape. Keep kPerRep well under 256.
    const int kReps = 31;          // far more than the probe's 7, per 12.3.b
    const int kPerRep = 32;

    auto timeLane = [&](int32_t backend) {
        const int64_t t0 = nowNs();
        for (int i = 0; i < kPerRep; ++i)
            c.launch("k", 1, 1, 1, 64, 1, 1, 0, 0, 0, backend, noKernel, nullptr);
        const int64_t dt = nowNs() - t0;
        c.pendingReset();          // outside the timed region
        c.gpuFlush();
        return static_cast<double>(dt) / kPerRep;
    };

    // Warm both lanes: the first launch through either pays for page faults
    // and cold branch predictors that no later launch does.
    timeLane(CAJ_GPU_BACKEND_CUDA);
    timeLane(CAJ_GPU_BACKEND_CPU);

    std::vector<double> cuda, host;
    for (int r = 0; r < kReps; ++r) {
        if (r % 2 == 0) {          // alternate, so a drifting load cannot
            cuda.push_back(timeLane(CAJ_GPU_BACKEND_CUDA));   // favour one arm
            host.push_back(timeLane(CAJ_GPU_BACKEND_CPU));
        } else {
            host.push_back(timeLane(CAJ_GPU_BACKEND_CPU));
            cuda.push_back(timeLane(CAJ_GPU_BACKEND_CUDA));
        }
    }

    const LaneStats cs = laneStats(cuda), hs = laneStats(host);
    double delta = 0, noise = 0;
    const bool resolvable = overheadIsResolvable(cs, hs, &delta, &noise);

    std::printf(" RESULT u12_ns_per_launch_cuda=%.1f spread=%.1f\n", cs.mean, cs.spread);
    std::printf(" RESULT u12_ns_per_launch_host_lane=%.1f spread=%.1f\n", hs.mean, hs.spread);
    std::printf(" RESULT u12_cuda_extra_ns_per_launch=%.1f\n", delta);
    std::printf(" RESULT u12_overhead_exceeds_noise=%s\n", resolvable ? "YES" : "NO");
    if (!resolvable) {
        std::printf(" NOTE the delta is smaller than the run-to-run spread "
                    "(%.1f ns); it bounds the cost, it does not measure it\n", noise);
    }

    // The backend must still have been the one under measurement: a degrade
    // mid-run (a second subscriber attaching, §5.4.3) would silently move the
    // lane to the host path and the numbers would describe nothing.
    EXPECT_NE(c.tracing(), 0) << "CUPTI stopped tracing during the measurement";
    EXPECT_STREQ(c.vtblName(CAJ_GPU_BACKEND_CUDA), "cuda");

    // A negative delta is not a faster profiler; it is noise wearing a sign.
    if (resolvable) EXPECT_GT(delta, 0.0);
}

// The verdict rule itself, run everywhere — the measurement above only runs on
// NVIDIA, so without this the rule that decides what gets published would
// never execute on any machine anyone looks at.
TEST(ProfilerCupti, anOverheadSmallerThanItsSpreadIsNotAMeasurement) {
    double delta = 0, noise = 0;

    // The audit probe's actual numbers: +957.5 ns against a 1320 ns spread.
    EXPECT_FALSE(overheadIsResolvable({9035.0, 1320.0}, {8077.5, 0.0},
                                      &delta, &noise));
    EXPECT_NEAR(delta, 957.5, 0.01);

    // Event bracketing at ~14 us against the same noise clears it easily.
    EXPECT_TRUE(overheadIsResolvable({22039.5, 1320.0}, {8077.5, 0.0},
                                     &delta, &noise));
    EXPECT_NEAR(delta, 13962.0, 0.01);

    // Both lanes' spreads count, not just the armed one: a quiet armed lane
    // against a noisy baseline is just as unresolved.
    EXPECT_FALSE(overheadIsResolvable({9035.0, 10.0}, {8077.5, 2000.0}, nullptr, nullptr));

    // Exactly at the noise floor is NOT resolved — a delta must clear it.
    EXPECT_FALSE(overheadIsResolvable({1100.0, 50.0}, {1000.0, 50.0}, nullptr, nullptr));
    EXPECT_TRUE(overheadIsResolvable({1101.0, 50.0}, {1000.0, 50.0}, nullptr, nullptr));

    // A lane that never ran has no statistics to report.
    const LaneStats empty = laneStats({});
    EXPECT_EQ(empty.mean, 0.0);
    EXPECT_EQ(empty.spread, 0.0);

    // The mean is over every rep and the spread is the full range, so one slow
    // rep widens the spread rather than quietly moving the mean past it.
    const LaneStats s = laneStats({100, 100, 100, 100, 500});
    EXPECT_NEAR(s.mean, 180.0, 0.01);
    EXPECT_NEAR(s.spread, 400.0, 0.01);
}
