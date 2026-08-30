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
#include <cstring>
#include <memory>
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
    // Unit 12's record path (12.1.a-e, g).
    int32_t (*kindAllowed)(int32_t) = nullptr;
    int32_t (*prefixBytes)(void) = nullptr;
    int32_t (*decodeKernel)(const void*, int64_t, int64_t*, int64_t*, int32_t*) = nullptr;
    int32_t (*noteSubscribe)(int32_t) = nullptr;
    int32_t (*degraded)(void) = nullptr;
    int32_t (*tsFirst)(void) = nullptr;
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
// kind is enabled. Registering it after leaves the first records in CUPTI's
// own domain, and run 32439821390 measured exactly that shape on WSL: the
// callback ACCEPTED and then ignored.
TEST(ProfilerCupti, theTimestampCallbackIsRegisteredBeforeAnyKindIsEnabled) {
    ASSERT_NE(cu().tsFirst, nullptr);
    cu().reset();
    EXPECT_EQ(cu().tsFirst(), 0) << "nothing registered yet on a fresh backend";
    cu().init();
    // Where CUPTI is absent this stays 0 and the claim is vacuous, so the
    // assertion is conditional on the backend actually being READY.
    if (cu().state() == CAJETA_CUPTI_READY && cu().hasTsCallback()) {
        EXPECT_EQ(cu().tsFirst(), 1)
            << "the callback must be registered before the first enable";
    } else {
        GTEST_SKIP() << "no CUPTI with a timestamp callback here; reason: "
                     << cu().reason();
    }
    cu().reset();
}

// ── 12.2.c/d — correlation ───────────────────────────────────────────────
//
// A kernel record does NOT carry our launch id. It carries CUPTI's own
// correlationId, and a SEPARATE record kind maps that to the external id we
// pushed. That is why the buffer parse is two-pass: pass one builds the map
// from EXTERNAL_CORRELATION records, pass two resolves kernels through it.
// A single pass would drop every kernel whose mapping record came later in
// the same buffer, which is not a rare ordering - it is the normal one.
//
// MEASURED against the same real header:
//   CUPTI_ACTIVITY_KIND_EXTERNAL_CORRELATION = 39
//   CUpti_ActivityExternalCorrelation:
//     kind=0  externalKind=4  externalId=8  correlationId=16  sizeof=24
namespace {

constexpr int32_t kKindExternalCorrelation = 39;
constexpr size_t  kXOffKind = 0, kXOffExtId = 8, kXOffCorr = 16;
constexpr int64_t kXRecBytes = 24;

struct FakeExternal {
    unsigned char bytes[24];
    FakeExternal(int32_t kind, uint64_t externalId, uint32_t corr) {
        std::memset(bytes, 0xCD, sizeof bytes);
        std::memcpy(bytes + kXOffKind,  &kind,       sizeof kind);
        std::memcpy(bytes + kXOffExtId, &externalId, sizeof externalId);
        std::memcpy(bytes + kXOffCorr,  &corr,       sizeof corr);
    }
};

} // namespace

TEST(ProfilerCupti, anExternalCorrelationRecordDecodes) {
    ASSERT_NE(cu().decodeExternal, nullptr);
    const FakeExternal r(kKindExternalCorrelation, 4242, 7);
    int32_t corr = -1; int64_t ext = -1;
    EXPECT_EQ(cu().decodeExternal(r.bytes, kXRecBytes, &corr, &ext), 1);
    EXPECT_EQ(corr, 7);
    EXPECT_EQ(ext, 4242);
}

TEST(ProfilerCupti, aTruncatedExternalCorrelationRecordIsRefused) {
    const FakeExternal r(kKindExternalCorrelation, 4242, 7);
    int32_t corr = -1; int64_t ext = -1;
    EXPECT_EQ(cu().decodeExternal(r.bytes, kXRecBytes - 1, &corr, &ext), 0);
    EXPECT_EQ(corr, -1) << "outputs untouched when nothing was decoded";
    // A kernel record must not be mistaken for a correlation record.
    const FakeRecord k(kKindConcurrentKernel, 1000, 4000, 7);
    EXPECT_EQ(cu().decodeExternal(k.bytes, sizeof k.bytes, &corr, &ext), 0);
}

TEST(ProfilerCupti, theCorrelationMapResolvesWhatWasNoted) {
    ASSERT_NE(cu().corrNote, nullptr);
    cu().corrReset();
    int64_t ext = -1;
    EXPECT_EQ(cu().corrLookup(7, &ext), 0) << "empty map resolves nothing";

    EXPECT_EQ(cu().corrNote(7, 4242), 1);
    EXPECT_EQ(cu().corrNote(8, 4243), 1);
    EXPECT_EQ(cu().corrLookup(7, &ext), 1);
    EXPECT_EQ(ext, 4242);
    EXPECT_EQ(cu().corrLookup(8, &ext), 1);
    EXPECT_EQ(ext, 4243);

    ext = -1;
    EXPECT_EQ(cu().corrLookup(9, &ext), 0) << "an id never noted must MISS";
    EXPECT_EQ(ext, -1) << "a miss must not write an output - a stale value here "
                          "would attribute a kernel to the wrong launch";
    cu().corrReset();
    EXPECT_EQ(cu().corrLookup(7, &ext), 0) << "reset clears the map";
}

// The map is fixed-size, because a buffer callback may not allocate. What
// matters is that overflow is COUNTED and lookups stay correct rather than
// wrapping onto a wrong answer: a kernel attributed to the wrong launch is
// worse than one not attributed at all.
TEST(ProfilerCupti, correlationOverflowIsCountedAndNeverWrapsOntoAWrongAnswer) {
    ASSERT_NE(cu().corrCapacity, nullptr);
    cu().corrReset();
    const int32_t cap = cu().corrCapacity();
    ASSERT_GT(cap, 0);
    const int64_t droppedBefore = cu().corrDropped();

    for (int32_t i = 0; i < cap; ++i) {
        ASSERT_EQ(cu().corrNote(i + 1, 1000 + i), 1) << "at i=" << i;
    }
    EXPECT_EQ(cu().corrDropped() - droppedBefore, 0) << "capacity must fit";

    EXPECT_EQ(cu().corrNote(cap + 1, 999999), 0) << "one past capacity is refused";
    EXPECT_EQ(cu().corrDropped() - droppedBefore, 1) << "and counted";

    int64_t ext = -1;
    EXPECT_EQ(cu().corrLookup(cap + 1, &ext), 0) << "the dropped id must MISS";
    EXPECT_EQ(cu().corrLookup(1, &ext), 1) << "and earlier entries survive";
    EXPECT_EQ(ext, 1000);
    cu().corrReset();
}

// 12.2.d — the launch chokepoint. With no CUPTI here these must be safe
// no-ops: the seam calls them on every launch, so a null deref would take
// down any program profiled on a box without CUDA.
TEST(ProfilerCupti, pushAndPopAreSafeWhenCuptiIsAbsent) {
    ASSERT_NE(cu().push, nullptr);
    cu().reset();
    if (cu().state() == CAJETA_CUPTI_READY) {
        GTEST_SKIP() << "CUPTI is present here; the absent path is the subject";
    }
    EXPECT_EQ(cu().tracing(), 0) << "not tracing without a backend";
    EXPECT_EQ(cu().push(1234), 0) << "push reports it did nothing";
    EXPECT_EQ(cu().pop(), 0);
    EXPECT_EQ(cu().push(0), 0) << "launch id 0 is not a launch";
    cu().reset();
}
