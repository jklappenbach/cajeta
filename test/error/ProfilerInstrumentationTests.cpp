// cajeta-profiler Unit 10 — exact instrumentation (spec §3).
//
// A separate TIER from sampling, not a replacement. Sampling answers "where
// does wall time go" from outside the program and costs it nothing; this
// answers "how many times, and how long exactly" and costs a probe pair per
// call. So the flag is opt-in, needs a rebuild, and every claim below is about
// what the BUILD contains — §3.2's promise is that a build without the flag is
// the build that existed before the flag did.
//
// The selection assertions read the IR rather than the trace on purpose
// (plan 10.4.a): "no probe was emitted" and "a probe was emitted and then
// ignored" look identical from anywhere downstream, and only one of them is
// the overhead reduction §3.8 is asking for.
#include "gtest/gtest.h"

#include <cstdint>
#include <string>
#include "../jit/JitTestHelper.h"
#include "ProfilerTraceRead.h"
#include <cstdio>
#include <cstdlib>
#include "cajeta/compile/CacheManifest.h"
#include "cajeta/compile/CompilerMode.h"

using cajeta::CompilerFlags;
using cajeta::Profiler;
using cajeta_test::CajetaJit;

namespace {

// Three methods, a known call pattern, no loops in the leaf: run(4) calls
// mid 4 times, mid calls leaf once each, so leaf is entered exactly 4 times
// and run exactly once.
const char* kSrc =
    "package test;\n"
    "public final class D {\n"
    "    public static int32 leaf(int32 x) { return x + 1; }\n"
    "    public static int32 mid(int32 x) { return D.leaf(x) + 1; }\n"
    "    public static int32 run(int32 n) {\n"
    "        int32 acc = 0;\n"
    "        int32 i = 0;\n"
    "        while (i < n) { acc = D.mid(acc); i = i + 1; }\n"
    "        return acc;\n"
    "    }\n"
    "}\n";

// §3.6 — `leaf` is force-inlined. The AlwaysInlinerPass runs even at O0, so
// `mid` ends up carrying leaf's body, probe pair and all. What the call count
// must NOT do is drop to zero because the callee stopped existing.
const char* kInlined =
    "package test;\n"
    "public final class I {\n"
    "    @Inline\n"
    "    public static int32 leaf(int32 x) { return x + 1; }\n"
    "    public static int32 mid(int32 x) { return I.leaf(x) + 1; }\n"
    "    public static int32 run(int32 n) {\n"
    "        int32 acc = 0;\n"
    "        int32 i = 0;\n"
    "        while (i < n) { acc = I.mid(acc); i = i + 1; }\n"
    "        return acc;\n"
    "    }\n"
    "}\n";

// §3.3 — the drop-elision shape from NoopDropElisionTests, compiled
// instrumented. A trivially-droppable stack local registers no drop entry, so
// dropCount stays 0; if the probe pair were not recognized as instrumentation,
// every such drop would look like it did something and the ~230x value-type
// drop tax would come back the moment a developer turned the profiler on.
const char* kTrivialDrop =
    "package test;\n"
    "import cajeta.time.Instant;\n"
    "public final class H {\n"
    "    public static int64 drops(int32 n) {\n"
    "        int64 acc = 0;\n"
    "        int32 i = 0;\n"
    "        Cajeta.dropCountReset();\n"
    "        while (i < n) {\n"
    "            Instant t = stack Instant((int64) i, 0);\n"
    "            Instant t2 = stack Instant(t.getEpochSecond() + 7, 0);\n"
    "            acc = acc + t2.getEpochSecond();\n"
    "            i = i + 1;\n"
    "        }\n"
    "        if (acc != (int64) n * ((int64) n - 1) / 2 + 7 * (int64) n) { return -1; }\n"
    "        return Cajeta.dropCount();\n"
    "    }\n"
    "}\n";

// A throw ACROSS probed frames. `mid` is entered from `run` and never returns
// normally, so its exit probe never runs — the unwind is the only way out.
const char* kThrows =
    "package test;\n"
    "import cajeta.error.Exception;\n"
    "public final class X {\n"
    "    public static int32 deep() { throw heap Exception(\"x\"); }\n"
    "    public static int32 mid() { return X.deep(); }\n"
    "    public static int32 run() {\n"
    "        try { return X.mid(); }\n"
    "        catch (Exception e) { return 7; }\n"
    "    }\n"
    "}\n";

CajetaJit::Options instrumented(const std::string& selection = "") {
    CajetaJit::Options o;
    o.profiler = Profiler::Instrument;
    o.profilerSelect = selection;
    o.captureIr = true;
    return o;
}

size_t countOf(const std::string& hay, const std::string& needle) {
    size_t n = 0, at = 0;
    while ((at = hay.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
    return n;
}

// The per-method descriptor global. Unique to ProfileCodegen — the runtime
// bitcode linked into every module defines the probe FUNCTIONS regardless, so
// searching for those would find a definition in an uninstrumented build too.
const char* kDescGlobal = ".cajeta.profmethod";

// How many descriptors the module DEFINES. Counted by definition line, not by
// name: LLVM uniquifies globals, so the second descriptor is
// `@.cajeta.profmethod.1` and a name match finds exactly one of however many
// there are.
// The text of one function's body, from its `define` line to the next.
std::string bodyOf(const std::string& ir, const std::string& defPrefix) {
    const size_t at = ir.find(defPrefix);
    if (at == std::string::npos) return "";
    const size_t end = ir.find("\ndefine ", at + 1);
    return ir.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

size_t descriptorDefs(const std::string& ir) {
    size_t n = 0, at = 0;
    const std::string def = "\n@" + std::string(kDescGlobal);
    while ((at = ir.find(def, at)) != std::string::npos) {
        const size_t eol = ir.find('\n', at + 1);
        const std::string line = ir.substr(at + 1, eol - at - 1);
        if (line.find(" = private global") != std::string::npos) ++n;
        at = (eol == std::string::npos) ? ir.size() : eol;
    }
    return n;
}

std::string flagValue(const CompilerFlags& f, const std::string& key) {
    for (const auto& p : cajeta::cacheFlagPairs(f, "exe", ""))
        if (p.first == key) return p.second;
    return "<absent>";
}

// The runtime accessors, resolved out of the JIT'd runtime copy — which is
// where a profiled program's counters actually live.
struct Instr {
    int64_t (*totalCalls)(void) = nullptr;
    int32_t (*methodCount)(void) = nullptr;
    const char* (*methodType)(int32_t) = nullptr;
    const char* (*methodName)(int32_t) = nullptr;
    int64_t (*methodCalls)(int32_t) = nullptr;
    int64_t (*methodInclusiveNs)(int32_t) = nullptr;
    int64_t (*methodOutsideCalls)(int32_t) = nullptr;
    int32_t (*isPresent)(void) = nullptr;
    const char* (*selection)(void) = nullptr;
    int32_t (*optLevel)(void) = nullptr;
    int64_t (*probePairs)(void) = nullptr;
    int64_t (*probeNs)(void) = nullptr;
    int64_t (*overheadNs)(void) = nullptr;
    void    (*reset)(void) = nullptr;
};

Instr bind(CajetaJit* jit) {
    Instr i;
    auto sym = [&](const char* n) { return jit->lookupRawSymbol(n); };
    i.totalCalls   = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_instr_total_calls"));
    i.methodCount  = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_instr_method_count"));
    i.methodType   = reinterpret_cast<const char* (*)(int32_t)>(sym("__cajeta_prof_instr_method_type"));
    i.methodName   = reinterpret_cast<const char* (*)(int32_t)>(sym("__cajeta_prof_instr_method_name"));
    i.methodCalls  = reinterpret_cast<int64_t (*)(int32_t)>(sym("__cajeta_prof_instr_method_calls"));
    i.methodInclusiveNs = reinterpret_cast<int64_t (*)(int32_t)>(sym("__cajeta_prof_instr_method_inclusive_ns"));
    i.methodOutsideCalls = reinterpret_cast<int64_t (*)(int32_t)>(sym("__cajeta_prof_instr_method_outside_calls"));
    i.isPresent    = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_instr_is_present"));
    i.selection    = reinterpret_cast<const char* (*)(void)>(sym("__cajeta_prof_instr_selection"));
    i.optLevel     = reinterpret_cast<int32_t (*)(void)>(sym("__cajeta_prof_instr_opt_level"));
    i.probePairs   = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_instr_probe_pairs"));
    i.probeNs      = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_instr_probe_ns"));
    i.overheadNs   = reinterpret_cast<int64_t (*)(void)>(sym("__cajeta_prof_instr_overhead_ns"));
    i.reset        = reinterpret_cast<void (*)(void)>(sym("__cajeta_prof_instr_reset"));
    return i;
}

// Index of test.D's `method`, or -1. Matched on TYPE and name: with no
// selection the stdlib is instrumented too, and `run` is not a rare name.
int32_t indexOf(const Instr& i, const std::string& method,
                const std::string& type = "test.D") {
    const int32_t n = i.methodCount();
    for (int32_t k = 0; k < n; ++k)
        if (method == i.methodName(k) && type == i.methodType(k)) return k;
    return -1;
}

int64_t callsOf(const Instr& i, const std::string& method,
                const std::string& type = "test.D") {
    const int32_t k = indexOf(i, method, type);
    return k < 0 ? -1 : i.methodCalls(k);
}

// The counting tests scope instrumentation to the test's own class. Not for
// correctness — an unscoped build counts the same calls — but because an
// unscoped one also probes every stdlib method the module pulled in, and the
// assertions are about test.D either way.
const char* kOnlyTestD = "include test.**\n";

} // namespace

// ── 10.1.a — probes at the prologue and on the return path ───────────────

TEST(ProfilerInstrumentation, instrumentedBuildProbesEveryMethod) {
    // Scoped to test.D so the counts are about three known methods. An
    // unscoped build instruments the stdlib the module pulled in too — which
    // is correct, and is what §3.1 asks for, but makes "one descriptor per
    // method" an assertion about four thousand of them.
    auto jit = CajetaJit::compile(kSrc, "test.D", instrumented(kOnlyTestD));
    ASSERT_TRUE(jit);
    const std::string& ir = jit->getModuleIr();

    // One descriptor per probed method, and the probe pair is balanced: a
    // build with more enters than exits leaks depth and mis-reports §3.11 for
    // every later call.
    const size_t descs  = descriptorDefs(ir);
    // `call`, not a bare name match: the module also carries a `declare` (or,
    // once the runtime bitcode is linked, a `define`) for each probe.
    const size_t enters = countOf(ir, "call i64 @__cajeta_prof_instr_enter(");
    const size_t exits  = countOf(ir, "call void @__cajeta_prof_instr_exit(");
    EXPECT_GE(descs, 3u) << "expected a descriptor for leaf, mid and run";
    EXPECT_GT(enters, 0u);
    // Exactly one enter per descriptor. This is also the structural half of
    // §3.6: the probe belongs to the CALLEE's body, so an inlined body carries
    // it along. Hoisting the pair to call sites would pass every count test on
    // un-inlined code and then silently lose every inlined call — and it would
    // show up here as many enters referencing one descriptor.
    EXPECT_EQ(enters, descs) << "an enter and a descriptor go together";
    EXPECT_GE(exits, enters)
        << "fewer exits than enters — some return path is unprobed";
}

// ── 10.1.b — §3.2, no flag, no probes ────────────────────────────────────

TEST(ProfilerInstrumentation, defaultBuildEmitsNoProbesAtAll) {
    CajetaJit::Options o;
    o.captureIr = true;   // profiler left at its default, Off
    auto jit = CajetaJit::compile(kSrc, "test.D", o);
    ASSERT_TRUE(jit);
    const std::string& ir = jit->getModuleIr();

    EXPECT_EQ(countOf(ir, kDescGlobal), 0u)
        << "an uninstrumented build carries instrumentation descriptors";
    EXPECT_EQ(countOf(ir, "call i64 @__cajeta_prof_instr_enter("), 0u);
    EXPECT_EQ(countOf(ir, "call void @__cajeta_prof_instr_exit("), 0u);
    EXPECT_EQ(countOf(ir, "__cajeta.profinstr.register"), 0u)
        << "the registration ctor ran in a build that asked for no profiler";
}

// ── 10.1.c — exact counts ────────────────────────────────────────────────

TEST(ProfilerInstrumentation, callCountsAreExactForAKnownPattern) {
    auto jit = CajetaJit::compile(kSrc, "test.D", instrumented(kOnlyTestD));
    ASSERT_TRUE(jit);
    Instr i = bind(jit.get());
    ASSERT_TRUE(i.reset && i.methodCount && i.methodCalls && i.methodName);

    auto run = jit->lookup<int32_t (*)(int32_t)>("run");
    ASSERT_TRUE(run);

    i.reset();
    EXPECT_EQ(run(4), 8);   // four rounds of +2

    EXPECT_EQ(callsOf(i, "run"), 1)
        << "run was entered once; instrumentation is exact, not sampled";
    EXPECT_EQ(callsOf(i, "mid"), 4);
    EXPECT_EQ(callsOf(i, "leaf"), 4);

    // And the reset is real: the same assertion must hold on a second run
    // rather than accumulating.
    i.reset();
    EXPECT_EQ(run(2), 4);
    EXPECT_EQ(callsOf(i, "mid"), 2);
}

// Inclusive time is measured, not modeled. A method that only calls a leaf
// still has to account for the leaf's time inside its own span.
TEST(ProfilerInstrumentation, inclusiveTimeIsAccumulated) {
    auto jit = CajetaJit::compile(kSrc, "test.D", instrumented(kOnlyTestD));
    ASSERT_TRUE(jit);
    Instr i = bind(jit.get());
    ASSERT_TRUE(i.reset);
    auto run = jit->lookup<int32_t (*)(int32_t)>("run");
    ASSERT_TRUE(run);

    i.reset();
    run(2000);

    const int32_t runAt = indexOf(i, "run");
    const int32_t leafAt = indexOf(i, "leaf");
    ASSERT_GE(runAt, 0);
    ASSERT_GE(leafAt, 0);
    const int64_t runNs = i.methodInclusiveNs(runAt);
    const int64_t leafNs = i.methodInclusiveNs(leafAt);
    EXPECT_GT(runNs, 0) << "no inclusive time recorded for a method that ran 2000 loops";
    EXPECT_GE(runNs, leafNs)
        << "the caller's inclusive span excludes its callee's — that is not inclusive";
}

// ── 10.1.f / 10.4.c — the cache key ──────────────────────────────────────

TEST(ProfilerInstrumentation, instrumentedAndPlainBuildsHaveDifferentCacheKeys) {
    CompilerFlags off, on;
    ASSERT_TRUE(cajeta::applyProfiler("off", off, nullptr));
    ASSERT_TRUE(cajeta::applyProfiler("instrument", on, nullptr));

    EXPECT_EQ(flagValue(off, "profiler"), "off");
    EXPECT_EQ(flagValue(on,  "profiler"), "instrument");
    EXPECT_NE(flagValue(off, "profiler"), flagValue(on, "profiler"))
        << "§3.7: an instrumented and a plain build would alias";
}

// The one with teeth (plan 10.4.c). Keying on the selection's PATH passes
// every other test in this file: two builds with different files differ, a
// build with no selection differs from one with a selection. It fails only
// here — a selection edited in place keeps its path, so the build silently
// hands back objects probed to the previous selection.
TEST(ProfilerInstrumentation, editingASelectionInPlaceChangesTheCacheKey) {
    CompilerFlags before, after;
    ASSERT_TRUE(cajeta::applyProfiler("instrument", before, nullptr));
    ASSERT_TRUE(cajeta::applyProfiler("instrument", after,  nullptr));
    // Same origin — the file was edited, not replaced.
    before.profilerSelectOrigin = after.profilerSelectOrigin = "/build/prof.sel";
    before.profilerSelect = "include dev.a.**\n";
    after.profilerSelect  = "include dev.a.**\nexclude dev.a.Noisy\n";

    EXPECT_NE(flagValue(before, "profiler-select"), flagValue(after, "profiler-select"))
        << "§3.10: the selection's CONTENTS must key the cache, not its path";
}

// The other direction, and the reason the origin is deliberately out of the
// key: two build roots that read the same selection from different paths are
// producing the same objects and must share them.
TEST(ProfilerInstrumentation, theSelectionsPathIsNotPartOfTheCacheKey) {
    CompilerFlags a, b;
    ASSERT_TRUE(cajeta::applyProfiler("instrument", a, nullptr));
    ASSERT_TRUE(cajeta::applyProfiler("instrument", b, nullptr));
    a.profilerSelect = b.profilerSelect = "include dev.a.**\n";
    a.profilerSelectOrigin = "/home/one/prof.sel";
    b.profilerSelectOrigin = "/srv/ci/prof.sel";

    const auto pairsA = cajeta::cacheFlagPairs(a, "exe", "");
    const auto pairsB = cajeta::cacheFlagPairs(b, "exe", "");
    EXPECT_EQ(pairsA, pairsB)
        << "the selection's path leaked into the cache key";
}

TEST(ProfilerInstrumentation, unknownProfilerValueErrorsAndNamesTheAcceptedSet) {
    CompilerFlags f;
    std::string err;
    EXPECT_FALSE(cajeta::applyProfiler("exact", f, &err));
    EXPECT_NE(err.find("off|instrument"), std::string::npos) << err;
    EXPECT_EQ(f.profiler, Profiler::Off) << "a rejected value still changed the flags";
}

// ── 10.4.a/b — selection acts at emission ────────────────────────────────

TEST(ProfilerInstrumentation, aClassOutsideTheSelectionGetsNoProbe) {
    auto jit = CajetaJit::compile(kSrc, "test.D",
                                  instrumented("include other.pkg.**\n"));
    ASSERT_TRUE(jit);
    const std::string& ir = jit->getModuleIr();
    EXPECT_EQ(countOf(ir, kDescGlobal), 0u)
        << "a class outside the selection carries probes; §3.8 makes the "
           "selection an EMISSION decision, so there is nothing to skip at "
           "runtime and no overhead to reduce";
}

TEST(ProfilerInstrumentation, aClassInsideTheSelectionKeepsItsProbes) {
    auto jit = CajetaJit::compile(kSrc, "test.D", instrumented("include test.**\n"));
    ASSERT_TRUE(jit);
    EXPECT_GT(countOf(jit->getModuleIr(), kDescGlobal), 0u);
}

TEST(ProfilerInstrumentation, anExcludedClassLosesItsProbesEvenWhenIncluded) {
    auto jit = CajetaJit::compile(
        kSrc, "test.D", instrumented("include test.**\nexclude test.D\n"));
    ASSERT_TRUE(jit);
    EXPECT_EQ(countOf(jit->getModuleIr(), kDescGlobal), 0u);
}

// ── 10.4.e/g — what the run records about itself ─────────────────────────

TEST(ProfilerInstrumentation, theBuildRecordsTheSelectionThatWasInForce) {
    auto jit = CajetaJit::compile(
        kSrc, "test.D", instrumented("include test.**\nexclude test.Other\n"));
    ASSERT_TRUE(jit);
    Instr i = bind(jit.get());
    ASSERT_TRUE(i.isPresent && i.selection);

    EXPECT_EQ(i.isPresent(), 1) << "instrumentation probes exist but nothing said so";
    // §3.12 — a profile that silently omits code reads as though that code
    // were free, so the selection travels with the run.
    EXPECT_STREQ(i.selection(), "include test.**; exclude test.Other");
}

TEST(ProfilerInstrumentation, theBuildRecordsItsOptimizationLevel) {
    auto jit = CajetaJit::compile(kSrc, "test.D", instrumented());
    ASSERT_TRUE(jit);
    Instr i = bind(jit.get());
    ASSERT_TRUE(i.optLevel);
    // §3.13/§14.11 — the flag pins no level, so the level is part of what
    // every number means and is never inferred by the reader. The JIT builds
    // at O0; the point of the assertion is that SOME level was recorded
    // rather than left at the "nobody said" sentinel.
    EXPECT_GE(i.optLevel(), 0)
        << "an instrumented run did not record the level it was built at";
}

// ── 10.3.b — §3.5, the measurement reports its own cost ──────────────────

TEST(ProfilerInstrumentation, theRunReportsWhatTheMeasurementCost) {
    auto jit = CajetaJit::compile(kSrc, "test.D", instrumented(kOnlyTestD));
    ASSERT_TRUE(jit);
    Instr i = bind(jit.get());
    ASSERT_TRUE(i.reset && i.probePairs && i.probeNs && i.overheadNs);
    auto run = jit->lookup<int32_t (*)(int32_t)>("run");
    ASSERT_TRUE(run);

    // Calibrate first: the calibration itself runs probe pairs, and it backs
    // its own out. Doing it before the reset would leave the program's count
    // negative, which is the failure this ordering pins.
    const int64_t perPair = i.probeNs();
    EXPECT_GE(perPair, 0);

    i.reset();
    run(100);
    const int64_t pairs = i.probePairs();
    EXPECT_GE(pairs, 201)
        << "100 mid + 100 leaf + 1 run is the floor; got " << pairs;
    EXPECT_EQ(i.overheadNs(), pairs * perPair)
        << "the reported overhead is not pairs x measured cost";
}

TEST(ProfilerInstrumentation, calibrationDoesNotChargeTheProgramForItsOwnProbes) {
    auto jit = CajetaJit::compile(kSrc, "test.D", instrumented(kOnlyTestD));
    ASSERT_TRUE(jit);
    Instr i = bind(jit.get());
    ASSERT_TRUE(i.reset && i.probePairs && i.probeNs && i.methodCount);

    i.probeNs();          // calibrate (idempotent — cached for the run)
    i.reset();
    EXPECT_EQ(i.probePairs(), 0);

    // And the calibration's scratch descriptor is never enumerated: it is not
    // a method of the program, and a consumer listing methods must not see it.
    const int32_t n = i.methodCount();
    for (int32_t k = 0; k < n; ++k) {
        EXPECT_STRNE(i.methodType(k), "<calibration>")
            << "the calibration descriptor leaked into the method list";
    }
}

// ── 10.4.d — §3.11, a call from outside the selection says so ────────────

TEST(ProfilerInstrumentation, aCallFromOutsideTheSelectionIsRecordedAsSuch) {
    // Only `leaf`'s owner is selected; `run` and `mid` live in the same class,
    // so instead of splitting classes we select the class and read the fact
    // from the ROOT call — run() is entered with no probed frame beneath it,
    // which is exactly the "caller outside the selection" state.
    auto jit = CajetaJit::compile(kSrc, "test.D", instrumented(kOnlyTestD));
    ASSERT_TRUE(jit);
    Instr i = bind(jit.get());
    ASSERT_TRUE(i.reset && i.methodOutsideCalls);
    auto run = jit->lookup<int32_t (*)(int32_t)>("run");
    ASSERT_TRUE(run);

    i.reset();
    run(3);

    const int32_t runAt = indexOf(i, "run");
    const int32_t leafAt = indexOf(i, "leaf");
    ASSERT_GE(runAt, 0);
    ASSERT_GE(leafAt, 0);
    const int64_t runOutside = i.methodOutsideCalls(runAt);
    const int64_t leafOutside = i.methodOutsideCalls(leafAt);
    EXPECT_EQ(runOutside, 1)
        << "run() was entered from outside the selection and did not say so";
    EXPECT_EQ(leafOutside, 0)
        << "leaf() was reached through two probed frames; calling that "
           "outside-selection would misreport the one fact §3.11 is about";
}

// ── 10.1.e — §3.6, an inlined method still records its call ──────────────

TEST(ProfilerInstrumentation, anInlinedMethodStillRecordsItsCall) {
    auto jit = CajetaJit::compile(kInlined, "test.I", instrumented(kOnlyTestD));
    ASSERT_TRUE(jit);
    Instr i = bind(jit.get());
    ASSERT_TRUE(i.reset && i.methodCount);
    auto run = jit->lookup<int32_t (*)(int32_t)>("run");
    ASSERT_TRUE(run);

    // First establish that leaf really was inlined INTO mid, so a pass on the
    // count below is evidence about inlining rather than about a build where
    // nothing happened to inline. The standalone `define` for leaf survives
    // either way — it has external linkage, so the inliner folds its call
    // sites without being able to delete it.
    const std::string& ir = jit->getModuleIr();
    const std::string mid = bodyOf(ir, "define i32 @\"test.I::mid(x:int32)\"");
    ASSERT_FALSE(mid.empty()) << "test.I::mid is not in the captured IR";
    ASSERT_EQ(mid.find("@\"test.I::leaf"), std::string::npos)
        << "@Inline did not fold leaf into mid, so this test proves nothing "
           "about §3.6; the AlwaysInlinerPass runs even at O0 (Optimizer.cpp)."
           "\nmid was:\n" << mid;
    EXPECT_NE(mid.find("call i64 @__cajeta_prof_instr_enter("), std::string::npos)
        << "leaf's body was inlined into mid but its probe did not come along";

    i.reset();
    EXPECT_EQ(run(4), 8);
    EXPECT_EQ(callsOf(i, "leaf", "test.I"), 4)
        << "an inlined method's calls vanished. The probe has to live in the "
           "callee's BODY so it travels with an inlined copy; hoisting it to "
           "call sites is what produces this failure.";
    EXPECT_EQ(callsOf(i, "mid", "test.I"), 4);
}

// ── 10.1.d — §3.3, the probes stay transparent to drop elision ───────────

TEST(ProfilerInstrumentation, probesDoNotDefeatTrivialDropElision) {
    auto jit = CajetaJit::compile(kTrivialDrop, "test.H", instrumented());
    ASSERT_TRUE(jit);
    auto drops = jit->lookup<int64_t (*)(int32_t)>("drops");
    ASSERT_TRUE(drops);

    // Same assertion NoopDropElisionTests makes on an uninstrumented build.
    // Enabling profiling must not change what the program does — and a
    // silently-reinstated drop entry per loop iteration is a ~230x tax that
    // would land in the very measurement the developer turned on to find it.
    EXPECT_EQ(drops(64), 0)
        << "trivial stack drops came back under --profiler=instrument; the "
           "probe pair is balanced and reclaims nothing, so isInstrumentationCall "
           "has to treat it as transparent";
}

// ── 10.2.f — the counts reach a trace from a REAL build ──────────────────
//
// tracegen + CI already judge the emitter itself with trace_processor, against
// synthetic descriptors. What that cannot check is the half above it: that a
// program compiled with --profiler=instrument, run, and drained produces a
// trace with its methods in it. The two halves fail independently — a correct
// emitter fed nothing writes a valid, empty file.
TEST(ProfilerInstrumentation, anInstrumentedRunWritesItsCountsToATrace) {
    auto jit = CajetaJit::compile(kSrc, "test.D", instrumented(kOnlyTestD));
    ASSERT_TRUE(jit);
    Instr i = bind(jit.get());
    ASSERT_TRUE(i.reset);
    auto run = jit->lookup<int32_t (*)(int32_t)>("run");
    ASSERT_TRUE(run);

    auto emit = reinterpret_cast<int64_t (*)(const char*)>(
        jit->lookupRawSymbol("__cajeta_prof_instr_only_to_trace"));
    auto varint = reinterpret_cast<cajeta_test_prof::VarintRead>(
        jit->lookupRawSymbol("__cajeta_pb_varint_read"));
    ASSERT_TRUE(emit);
    ASSERT_TRUE(varint);

    const char* tmp = std::getenv("TMPDIR");
    const std::string path =
        std::string(tmp && tmp[0] ? tmp : ".") + "/cajeta-u10-instr.pftrace";
    std::remove(path.c_str());

    i.reset();
    run(5);
    const int64_t packets = emit(path.c_str());
    EXPECT_GT(packets, 0) << "an instrumented run drained to nothing";

    const auto tracks = cajeta_test_prof::readTracks(path.c_str(), varint);
    bool sawInstr = false;
    for (const auto& t : tracks)
        if (t.name == "cajeta.instrumentation") sawInstr = true;
    EXPECT_TRUE(sawInstr)
        << "the trace has no instrumentation track; §3.4 wants the tier "
           "identifiable, and a consumer identifies it by where the records live";
    std::remove(path.c_str());
}

// ── §3.11 — a throw must not leave the probe depth high ──────────────────
//
// An unwound frame never runs its exit probe, so the depth the enter raised is
// only ever brought back down by the exception machinery's watermark. Without
// that restore, `run()` returns fine and everything LOOKS right — and then
// every subsequent root call is quietly no longer counted as arriving from
// outside the selection, because the depth never came back to 0. It grows with
// each throw, so the error compounds instead of washing out.
//
// This is the same shape as the 6.4.C lambda leave that eroded the shadow
// stack a frame per call: an unbalanced counter whose symptom appears far from
// its cause, and long after.
TEST(ProfilerInstrumentation, aThrowRestoresTheProbeDepth) {
    auto jit = CajetaJit::compile(kThrows, "test.X", instrumented("include test.**\n"));
    ASSERT_TRUE(jit);
    Instr i = bind(jit.get());
    ASSERT_TRUE(i.reset && i.methodOutsideCalls);
    auto run = jit->lookup<int32_t (*)()>("run");
    ASSERT_TRUE(run);

    i.reset();
    ASSERT_EQ(run(), 7) << "the throw must be caught; this tests the unwind path";
    ASSERT_EQ(run(), 7);
    ASSERT_EQ(run(), 7);

    // Every one of the three calls came from the test harness — outside the
    // selection by definition. A leaked depth makes calls 2 and 3 look nested.
    const int32_t runAt = indexOf(i, "run", "test.X");
    ASSERT_GE(runAt, 0);
    EXPECT_EQ(i.methodCalls(runAt), 3);
    EXPECT_EQ(i.methodOutsideCalls(runAt), 3)
        << "the probe depth was not restored on unwind, so a root call after a "
           "throw looks as though it had a probed ancestor — the fabricated "
           "call edge §3.11 exists to forbid";

    // And `deep`, which only ever exits by throwing, still records its calls:
    // the ENTER ran even though the exit never did.
    EXPECT_EQ(callsOf(i, "deep", "test.X"), 3)
        << "a method that only ever exits by throwing lost its entry count";
}
