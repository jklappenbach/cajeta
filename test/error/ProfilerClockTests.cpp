// cajeta-profiler Unit 9 — clock correlation (plan 9.1.b/c/d, 9.2.a;
// spec §6.6, §6.7, §11.3, §11.4).
//
// Every failure this unit guards against returns success and plausible
// numbers, so the tests drive the correlation code through inputs whose right
// answer is known in advance and check the number, not the status code.
//
// The device clock here is SYNTHETIC, and deliberately so: plan 9.1.b records
// that the CPU backend cannot exercise any of this, because its offset and its
// drift are both exactly zero. A correlation module that returned "no offset,
// no drift" unconditionally would pass every CPU-backend test ever written.
// The synthetic domain has an offset and a drift the test picked, so a wrong
// answer is a wrong number rather than a missing feature.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "../../runtime/native/cajeta_prof_abi.h"
#include <cmath>
#include <cstdint>
using cajeta_test::CajetaJit;

namespace {

struct Clock {
    std::unique_ptr<CajetaJit> jit;
    int32_t (*reset)(int32_t) = nullptr;
    int32_t (*set_period)(int32_t, double) = nullptr;
    double  (*period)(int32_t) = nullptr;
    int32_t (*sample)(int32_t, int64_t, int64_t, int64_t) = nullptr;
    int32_t (*samples)(int32_t) = nullptr;
    int32_t (*rejected)(int32_t) = nullptr;
    int32_t (*valid)(int32_t) = nullptr;
    int64_t (*to_host)(int32_t, int64_t) = nullptr;
    double  (*drift_ppm)(int32_t) = nullptr;
    int64_t (*offset_ns)(int32_t) = nullptr;
    int32_t (*confidence)(int32_t) = nullptr;
    int32_t (*set_dispersion_cap)(int64_t) = nullptr;
    int32_t (*check_span)(int32_t, int64_t, int64_t) = nullptr;
};

Clock& clk() {
    static Clock c = [] {
        Clock x;
        x.jit = CajetaJit::compile(
            "package test;\npublic final class C {\n"
            "    public static int32 run() { return 1; }\n}\n", "test.C");
        auto sym = [&](const char* n) { return x.jit->lookupRawSymbol(n); };
        x.reset = reinterpret_cast<decltype(x.reset)>(sym("__cajeta_prof_clock_reset"));
        x.set_period = reinterpret_cast<decltype(x.set_period)>(sym("__cajeta_prof_clock_set_period"));
        x.period = reinterpret_cast<decltype(x.period)>(sym("__cajeta_prof_clock_period"));
        x.sample = reinterpret_cast<decltype(x.sample)>(sym("__cajeta_prof_clock_sample"));
        x.samples = reinterpret_cast<decltype(x.samples)>(sym("__cajeta_prof_clock_samples"));
        x.rejected = reinterpret_cast<decltype(x.rejected)>(sym("__cajeta_prof_clock_rejected"));
        x.valid = reinterpret_cast<decltype(x.valid)>(sym("__cajeta_prof_clock_valid"));
        x.to_host = reinterpret_cast<decltype(x.to_host)>(sym("__cajeta_prof_clock_to_host"));
        x.drift_ppm = reinterpret_cast<decltype(x.drift_ppm)>(sym("__cajeta_prof_clock_drift_ppm"));
        x.offset_ns = reinterpret_cast<decltype(x.offset_ns)>(sym("__cajeta_prof_clock_offset_ns"));
        x.confidence = reinterpret_cast<decltype(x.confidence)>(sym("__cajeta_prof_clock_confidence"));
        x.set_dispersion_cap = reinterpret_cast<decltype(x.set_dispersion_cap)>(sym("__cajeta_prof_clock_set_dispersion_cap"));
        x.check_span = reinterpret_cast<decltype(x.check_span)>(sym("__cajeta_prof_clock_check_span"));
        return x;
    }();
    return c;
}

constexpr int32_t D = CAJETA_CLOCK_DOMAIN_SYNTH;

// A device whose clock ticks every `period` host nanoseconds NOMINALLY, runs
// `driftPpm` fast or slow against that nominal rate, and whose tick zero sits
// `offsetNs` away from the host epoch. Modelled on the measured reference
// device in spec §6.6: −15 ppm, raw ticks 104.6 s from the host clock.
struct SyntheticDevice {
    double  period;      // nominal host ns per device tick (what the driver claims)
    double  driftPpm;    // true rate error against that nominal
    int64_t offsetNs;    // host ns at device tick 0

    int64_t hostFor(int64_t ticks) const {
        return offsetNs + (int64_t) llround((double) ticks * period
                                            * (1.0 + driftPpm * 1e-6));
    }
    int64_t ticksFor(int64_t hostNs) const {
        return (int64_t) llround((double) (hostNs - offsetNs)
                                 / (period * (1.0 + driftPpm * 1e-6)));
    }
};

// Feed `n` calibration sandwiches spaced `spacingNs` apart, each with
// `dispersionNs` between the bracketing host reads.
void calibrate(const SyntheticDevice& dev, int32_t n, int64_t spacingNs,
               int64_t dispersionNs = 200) {
    for (int32_t i = 0; i < n; ++i) {
        int64_t hostMid = dev.offsetNs + 1000000 + (int64_t) i * spacingNs;
        int64_t ticks = dev.ticksFor(hostMid);
        clk().sample(D, hostMid - dispersionNs / 2, ticks,
                     hostMid + dispersionNs / 2);
    }
}

} // namespace

// 9.1.b — the whole point of the unit. A device 40 ppm fast whose epoch sits
// 104.6 s from the host's must come back as 40 ppm and 104.6 s, not as a
// plausible timeline built on "close enough".
TEST(ProfilerClock, syntheticOffsetAndDriftAreRecovered) {
    ASSERT_NE(clk().reset, nullptr) << "__cajeta_prof_clock_reset not linked";
    ASSERT_EQ(clk().reset(D), 1);
    ASSERT_EQ(clk().set_period(D, 2.5), 1);          // 400 MHz device counter

    SyntheticDevice dev{2.5, 40.0, 104600000000LL};  // +40 ppm, 104.6 s offset
    calibrate(dev, 16, /*spacingNs=*/100000000LL);   // 16 samples over 1.6 s

    EXPECT_EQ(clk().valid(D), 1);
    EXPECT_EQ(clk().samples(D), 16);
    EXPECT_NEAR(clk().drift_ppm(D), 40.0, 1.0);
    EXPECT_NEAR((double) clk().offset_ns(D), 104600000000.0, 5000.0);

    // The mapping itself, which is what every consumer actually uses.
    for (int64_t t : {0LL, 1000000LL, 4000000000LL}) {
        EXPECT_NEAR((double) clk().to_host(D, t), (double) dev.hostFor(t), 5000.0)
            << "device tick " << t;
    }
}

// A single calibration is not enough (§6.6): the reference device drifts about
// 54 ms per hour, so a fit that recovered only the offset would be 54 ms wrong
// an hour in. Assert the drift term is genuinely doing work — an offset-only
// fit lands ~40 us off at 1 s and ~4 ms off at 100 s.
TEST(ProfilerClock, driftTermIsLoadBearingFarFromTheAnchor) {
    ASSERT_EQ(clk().reset(D), 1);
    ASSERT_EQ(clk().set_period(D, 2.5), 1);
    SyntheticDevice dev{2.5, 40.0, 104600000000LL};
    calibrate(dev, 16, 100000000LL);

    const int64_t farTicks = 40000000000LL;          // 100 s of device time
    const int64_t truth = dev.hostFor(farTicks);
    EXPECT_NEAR((double) clk().to_host(D, farTicks), (double) truth, 50000.0);

    // What an offset-only fit would have produced, for contrast.
    const int64_t offsetOnly = clk().offset_ns(D)
                             + (int64_t) llround((double) farTicks * 2.5);
    EXPECT_GT(std::llabs(offsetOnly - truth), 1000000LL)
        << "the synthetic drift is too small to distinguish the two fits";
}

// §6.7 — a sample taken while the device was in an unfavourable power state
// has a wide host-read sandwich, and folding it in tilts the fit. Rejected
// samples must not reach the fit, and must be counted where a consumer can
// see them.
TEST(ProfilerClock, poorQualitySamplesAreRejectedNotAveragedIn) {
    ASSERT_EQ(clk().reset(D), 1);
    ASSERT_EQ(clk().set_period(D, 2.5), 1);
    ASSERT_EQ(clk().set_dispersion_cap(50000), 1);   // 50 us

    SyntheticDevice dev{2.5, 40.0, 104600000000LL};
    calibrate(dev, 8, 100000000LL, /*dispersionNs=*/200);
    const double cleanDrift = clk().drift_ppm(D);
    const int32_t cleanCount = clk().samples(D);

    // A sample whose device read is 3 ms adrift, bracketed by a 5 ms sandwich
    // that says so. It is a lie the fit must not believe.
    int64_t hostMid = dev.offsetNs + 2000000000LL;
    clk().sample(D, hostMid - 2500000, dev.ticksFor(hostMid) + 1200000,
                 hostMid + 2500000);

    EXPECT_EQ(clk().samples(D), cleanCount) << "a rejected sample entered the fit";
    EXPECT_EQ(clk().rejected(D), 1);
    EXPECT_NEAR(clk().drift_ppm(D), cleanDrift, 0.5);
}

// §6.7 — the retry is BOUNDED. A device that never yields a good sample must
// leave the domain invalid and the caller running, not spinning.
TEST(ProfilerClock, allBadSamplesLeaveTheDomainInvalidRatherThanLooping) {
    ASSERT_EQ(clk().reset(D), 1);
    ASSERT_EQ(clk().set_period(D, 2.5), 1);
    ASSERT_EQ(clk().set_dispersion_cap(50000), 1);

    SyntheticDevice dev{2.5, 40.0, 104600000000LL};
    for (int32_t i = 0; i < 32; ++i) {
        int64_t hostMid = dev.offsetNs + 1000000 + (int64_t) i * 100000000LL;
        EXPECT_EQ(clk().sample(D, hostMid - 5000000, dev.ticksFor(hostMid),
                               hostMid + 5000000),
                  CAJETA_CLOCK_REJECT_DISPERSION);
    }
    EXPECT_EQ(clk().valid(D), 0);
    EXPECT_EQ(clk().samples(D), 0);
    EXPECT_EQ(clk().rejected(D), 32);
    // §11.6 — an uncorrelated domain says so rather than offering a timeline.
    EXPECT_EQ(clk().confidence(D), 0);
}

// 9.1.d / §11.4 — a driver reporting an implausible period is rejected rather
// than used. Zero and negative are the shapes actually seen; the upper bound
// catches a units mix-up (seconds handed over as nanoseconds).
TEST(ProfilerClock, implausibleTimestampPeriodIsRejected) {
    ASSERT_EQ(clk().reset(D), 1);
    for (double bad : {0.0, -1.0, -2.5, 1e12, 1e-12}) {
        EXPECT_EQ(clk().set_period(D, bad), 0) << "period " << bad << " accepted";
    }
    EXPECT_EQ(clk().valid(D), 0);
    for (double ok : {1.0, 2.5, 1000.0}) {
        EXPECT_EQ(clk().set_period(D, ok), 1) << "period " << ok << " rejected";
        EXPECT_DOUBLE_EQ(clk().period(D), ok);
    }
}

// 9.1.c / §11.3 — non-monotonic and implausible spans are FLAGGED, not
// dropped and not silently rendered. A dropped span looks like idle time; a
// rendered one looks like a measurement.
TEST(ProfilerClock, nonMonotonicAndImplausibleSpansAreFlagged) {
    ASSERT_EQ(clk().reset(D), 1);
    ASSERT_EQ(clk().set_period(D, 2.5), 1);
    SyntheticDevice dev{2.5, 40.0, 104600000000LL};
    calibrate(dev, 16, 100000000LL);

    const int64_t base = dev.offsetNs + 1000000000LL;

    EXPECT_EQ(clk().check_span(D, base, base + 500000), CAJETA_SPAN_OK);
    EXPECT_EQ(clk().check_span(D, base, base), CAJETA_SPAN_OK) << "a zero-length span is legal";

    EXPECT_TRUE(clk().check_span(D, base + 500000, base) & CAJETA_SPAN_NEGATIVE);
    // An hour-long kernel is a broken timestamp, not a slow kernel.
    EXPECT_TRUE(clk().check_span(D, base, base + 3600000000000LL)
                & CAJETA_SPAN_IMPLAUSIBLE);

    // Monotonicity is across spans, not within one: a span starting before the
    // previous span started is the device clock having gone backwards.
    EXPECT_EQ(clk().check_span(D, base + 1000000, base + 1500000), CAJETA_SPAN_OK);
    EXPECT_TRUE(clk().check_span(D, base + 900000, base + 950000)
                & CAJETA_SPAN_NONMONOTONIC);
}

// §11.6 — before any calibration there is no timeline to offer, and asking for
// one must say so rather than returning the raw ticks as if they were host ns.
TEST(ProfilerClock, anUncorrelatedDomainRefusesToConvert) {
    ASSERT_EQ(clk().reset(D), 1);
    EXPECT_EQ(clk().valid(D), 0);
    EXPECT_EQ(clk().confidence(D), 0);
    EXPECT_EQ(clk().to_host(D, 123456789LL), 0LL);
    EXPECT_TRUE(clk().check_span(D, 1000, 2000) & CAJETA_SPAN_UNCORRELATED);
}

// --- the retry bound belongs to the module, not to each caller -------------
//
// §6.7 requires a BOUNDED retry. Leaving the bound to whoever calls sample()
// makes it a convention, and a convention is what the next backend forgets:
// a device in an unfavourable power state would then hang the profiler at
// startup, which is the one failure a profiler must never cause.

namespace {
// A device that yields `goodAfter` bad sandwiches before settling down.
struct FlakyReader {
    SyntheticDevice dev{2.5, 40.0, 104600000000LL};
    int32_t calls = 0;
    int32_t goodAfter = 0;       // INT32_MAX => never good
    int64_t spacingNs = 100000000LL;

    static int32_t fn(int64_t* before, int64_t* ticks, int64_t* after, void* user) {
        auto* r = static_cast<FlakyReader*>(user);
        int64_t mid = r->dev.offsetNs + 1000000 + (int64_t) r->calls * r->spacingNs;
        int64_t half = (r->calls >= r->goodAfter) ? 100 : 5000000;   // 200 ns vs 10 ms
        r->calls++;
        *before = mid - half;
        *ticks = r->dev.ticksFor(mid);
        *after = mid + half;
        return 1;
    }
};
using ClockReadFn = int32_t (*)(int64_t*, int64_t*, int64_t*, void*);
} // namespace

TEST(ProfilerClock, calibrationStopsAtTheAttemptBoundWhenNoSampleIsEverGood) {
    auto calibrateFn = reinterpret_cast<int32_t (*)(int32_t, ClockReadFn, void*,
                                                    int32_t, int32_t)>(
        clk().jit->lookupRawSymbol("__cajeta_prof_clock_calibrate"));
    ASSERT_NE(calibrateFn, nullptr) << "__cajeta_prof_clock_calibrate not linked";

    ASSERT_EQ(clk().reset(D), 1);
    ASSERT_EQ(clk().set_period(D, 2.5), 1);
    ASSERT_EQ(clk().set_dispersion_cap(50000), 1);

    FlakyReader r;
    r.goodAfter = INT32_MAX;                       // never settles
    EXPECT_EQ(calibrateFn(D, &FlakyReader::fn, &r, /*want=*/8, /*maxAttempts=*/12), 0);
    EXPECT_EQ(r.calls, 12) << "the attempt bound was not honoured";
    EXPECT_EQ(clk().valid(D), 0);
    EXPECT_EQ(clk().confidence(D), 0);
}

TEST(ProfilerClock, calibrationRidesOutEarlyBadSamplesAndStopsOnceSatisfied) {
    auto calibrateFn = reinterpret_cast<int32_t (*)(int32_t, ClockReadFn, void*,
                                                    int32_t, int32_t)>(
        clk().jit->lookupRawSymbol("__cajeta_prof_clock_calibrate"));
    ASSERT_NE(calibrateFn, nullptr);

    ASSERT_EQ(clk().reset(D), 1);
    ASSERT_EQ(clk().set_period(D, 2.5), 1);
    ASSERT_EQ(clk().set_dispersion_cap(50000), 1);

    FlakyReader r;
    r.goodAfter = 3;                               // three bad, then good
    EXPECT_EQ(calibrateFn(D, &FlakyReader::fn, &r, /*want=*/8, /*maxAttempts=*/40), 8);
    EXPECT_EQ(r.calls, 11) << "it kept sampling after the target was met";
    EXPECT_EQ(clk().samples(D), 8);
    EXPECT_EQ(clk().rejected(D), 3);
    EXPECT_EQ(clk().valid(D), 1);
    EXPECT_NEAR(clk().drift_ppm(D), 40.0, 1.0);
}

// --- 9.2.d / 9.2.e: what reaches the trace --------------------------------
//
// Read the bytes. A wrong field number produces a file that is valid protobuf
// and still does not carry what it claims, and every in-process assertion
// passes anyway — the failure the vendored proto in third_party/perfetto
// exists to prevent.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Pb {
    const uint8_t* p;
    const uint8_t* end;
    bool next(uint32_t* field, uint32_t* wire, const uint8_t** data,
              uint64_t* len, uint64_t* value) {
        if (p >= end) return false;
        uint64_t key = varint();
        *field = (uint32_t) (key >> 3);
        *wire = (uint32_t) (key & 7);
        switch (*wire) {
            case 0: *value = varint(); *data = nullptr; *len = 0; break;
            case 1: *value = 0; memcpy(value, p, 8); p += 8; *data = nullptr; *len = 8; break;
            case 2: *len = varint(); *data = p; p += *len; *value = 0; break;
            case 5: *value = 0; memcpy(value, p, 4); p += 4; *data = nullptr; *len = 4; break;
            default: return false;
        }
        return p <= end;
    }
    uint64_t varint() {
        uint64_t r = 0; int s = 0;
        while (p < end) { uint8_t b = *p++; r |= (uint64_t) (b & 0x7f) << s;
                          if (!(b & 0x80)) break; s += 7; }
        return r;
    }
};

std::vector<uint8_t> slurp(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    uint8_t buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return out;
}

struct Snapshot { std::vector<std::pair<uint64_t, uint64_t>> clocks; };  // id -> ts

struct TraceFacts {
    std::vector<Snapshot> snapshots;
    std::vector<std::pair<std::string, int64_t>> annotations;
    std::vector<std::pair<std::string, std::string>> strings;
};

TraceFacts readTrace(const std::string& path) {
    TraceFacts facts;
    auto bytes = slurp(path);
    Pb top{bytes.data(), bytes.data() + bytes.size()};
    uint32_t f, w; const uint8_t* d; uint64_t len, v;
    while (top.next(&f, &w, &d, &len, &v)) {
        if (f != 1 || w != 2) continue;                       // Trace.packet
        Pb pkt{d, d + len};
        uint32_t pf, pw; const uint8_t* pd; uint64_t pl, pv;
        while (pkt.next(&pf, &pw, &pd, &pl, &pv)) {
            if (pf == 6 && pw == 2) {                         // clock_snapshot
                Snapshot s;
                Pb cs{pd, pd + pl};
                uint32_t cf, cw; const uint8_t* cd; uint64_t cl, cv;
                while (cs.next(&cf, &cw, &cd, &cl, &cv)) {
                    if (cf != 1 || cw != 2) continue;         // ClockSnapshot.clocks
                    Pb ck{cd, cd + cl};
                    uint32_t kf, kw; const uint8_t* kd; uint64_t kl, kv;
                    uint64_t id = 0, ts = 0;
                    while (ck.next(&kf, &kw, &kd, &kl, &kv)) {
                        if (kf == 1) id = kv;
                        else if (kf == 2) ts = kv;
                    }
                    s.clocks.emplace_back(id, ts);
                }
                facts.snapshots.push_back(s);
            } else if (pf == 11 && pw == 2) {                 // track_event
                Pb te{pd, pd + pl};
                uint32_t tf, tw; const uint8_t* td; uint64_t tl, tv;
                while (te.next(&tf, &tw, &td, &tl, &tv)) {
                    if (tf != 4 || tw != 2) continue;         // debug_annotations
                    Pb da{td, td + tl};
                    uint32_t af, aw; const uint8_t* ad; uint64_t al, av;
                    std::string name, sval; int64_t val = 0; bool isStr = false;
                    while (da.next(&af, &aw, &ad, &al, &av)) {
                        if (af == 10 && ad) name.assign((const char*) ad, al);
                        else if (af == 4) val = (int64_t) av;
                        else if (af == 6 && ad) { sval.assign((const char*) ad, al); isStr = true; }
                    }
                    if (name.empty()) continue;
                    if (isStr) facts.strings.emplace_back(name, sval);
                    else facts.annotations.emplace_back(name, val);
                }
            }
        }
    }
    return facts;
}

std::string tracePath(const char* leaf) {
    const char* base = getenv("TMPDIR");
    std::string dir = (base && *base) ? base : ".";
    return dir + "/cajeta_clock_" + leaf + ".pftrace";
}

// Every test in this file shares one process, and a calibrated domain stays
// calibrated — which is correct for a real run and makes a count-the-domains
// assertion depend on test order. Reset the whole space so these read the same
// whether they run alone or after the rest.
void resetAllDomains() {
    for (int32_t d = 0; d < CAJETA_CLOCK_MAX_DOMAINS; ++d) clk().reset(d);
}

CajetaGpuEvent gpuEvent(int32_t backend, int64_t launch, int64_t ret,
                        int64_t devStart, int64_t devEnd, int32_t tier) {
    CajetaGpuEvent e{};
    e.launch_id = 1;
    e.host_launch_ns = launch;
    e.host_return_ns = ret;
    e.dev_start_ns = devStart;
    e.dev_end_ns = devEnd;
    e.kernel_name = "test.K.add";
    e.backend = backend;
    e.tier = tier;
    return e;
}

} // namespace

// 9.2.e / §7.5 — the calibration that produced the timeline is IN the trace.
// Without it a reader has converted timestamps and no way to check them, which
// is the position §11.6 refuses to leave a consumer in.
TEST(ProfilerClock, eachCalibrationEmitsAClockSnapshotIntoTheTrace) {
    auto& j = *clk().jit;
    auto snapClear = reinterpret_cast<int32_t (*)()>(j.lookupRawSymbol("__cajeta_prof_clock_snapshot_clear"));
    auto snapCount = reinterpret_cast<int32_t (*)()>(j.lookupRawSymbol("__cajeta_prof_clock_snapshot_count"));
    auto calibrateFn = reinterpret_cast<int32_t (*)(int32_t, ClockReadFn, void*, int32_t, int32_t)>(
        j.lookupRawSymbol("__cajeta_prof_clock_calibrate"));
    auto toTrace = reinterpret_cast<int64_t (*)(const CajetaGpuEvent*, int64_t, const char*)>(
        j.lookupRawSymbol("__cajeta_prof_gpu_events_to_trace"));
    ASSERT_NE(snapClear, nullptr) << "__cajeta_prof_clock_snapshot_clear not linked";
    ASSERT_NE(toTrace, nullptr);

    constexpr int32_t CPU = 3;                       // the CPU backend's domain
    snapClear();
    ASSERT_EQ(clk().reset(CPU), 1);
    ASSERT_EQ(clk().set_period(CPU, 2.5), 1);
    ASSERT_EQ(clk().set_dispersion_cap(50000), 1);

    FlakyReader r;
    r.goodAfter = 0;
    ASSERT_EQ(calibrateFn(CPU, &FlakyReader::fn, &r, 6, 20), 6);
    ASSERT_EQ(snapCount(), 1) << "one calibration round, one snapshot";

    const int64_t launch = 1000000000LL;
    CajetaGpuEvent evs[1] = {
        gpuEvent(CPU, launch, launch + 8000000LL, launch + 1000, launch + 6000000LL,
                 CAJETA_PROF_TIER_DEVICE)};
    const std::string path = tracePath("snapshot");
    ASSERT_GT(toTrace(evs, 1, path.c_str()), 0);

    auto facts = readTrace(path);
    ASSERT_EQ(facts.snapshots.size(), 1u) << "no ClockSnapshot packet in the trace";
    const auto& cs = facts.snapshots[0];
    ASSERT_EQ(cs.clocks.size(), 2u) << "a snapshot pairs the host clock with the device's";

    bool sawHost = false, sawDevice = false;
    for (auto& [id, ts] : cs.clocks) {
        if (id == 3) { sawHost = true; EXPECT_GT(ts, 0u); }        // BUILTIN MONOTONIC
        if (id == (uint64_t) (CAJETA_CLOCK_PERFETTO_BASE_ID + CPU)) {
            sawDevice = true;
            EXPECT_GT(ts, 0u) << "the device side must carry raw TICKS, not zero";
        }
    }
    EXPECT_TRUE(sawHost) << "host clock id 3 (MONOTONIC) absent";
    EXPECT_TRUE(sawDevice) << "device clock id " << (CAJETA_CLOCK_PERFETTO_BASE_ID + CPU)
                           << " absent — user clock ids live in [64,127]";
    ::remove(path.c_str());
}

// 9.2.d / §10.6 — tier and confidence ride on the MEASUREMENT. A run can mix
// them (one backend demoted, another not), so a run-level note would leave a
// developer inferring whether the span in front of them was degraded.
TEST(ProfilerClock, everyDeviceSpanCarriesItsTierAndConfidence) {
    auto& j = *clk().jit;
    auto toTrace = reinterpret_cast<int64_t (*)(const CajetaGpuEvent*, int64_t, const char*)>(
        j.lookupRawSymbol("__cajeta_prof_gpu_events_to_trace"));
    auto calibrateFn = reinterpret_cast<int32_t (*)(int32_t, ClockReadFn, void*, int32_t, int32_t)>(
        j.lookupRawSymbol("__cajeta_prof_clock_calibrate"));
    ASSERT_NE(toTrace, nullptr);

    constexpr int32_t CPU = 3;
    ASSERT_EQ(clk().reset(CPU), 1);
    ASSERT_EQ(clk().set_period(CPU, 2.5), 1);
    ASSERT_EQ(clk().set_dispersion_cap(50000), 1);
    FlakyReader r; r.goodAfter = 0;
    ASSERT_EQ(calibrateFn(CPU, &FlakyReader::fn, &r, 8, 20), 8);
    const int32_t expected = clk().confidence(CPU);
    ASSERT_GT(expected, 0);

    const int64_t launch = 1000000000LL;
    CajetaGpuEvent evs[1] = {
        gpuEvent(CPU, launch, launch + 8000000LL, launch + 1000, launch + 6000000LL,
                 CAJETA_PROF_TIER_EVENT)};
    const std::string path = tracePath("anno");
    ASSERT_GT(toTrace(evs, 1, path.c_str()), 0);

    auto facts = readTrace(path);
    bool sawTier = false, sawConf = false, sawIntegrity = false;
    for (auto& [name, val] : facts.annotations) {
        if (name == "tier") { sawTier = true; EXPECT_EQ(val, CAJETA_PROF_TIER_EVENT); }
        if (name == "clock_confidence") { sawConf = true; EXPECT_EQ(val, expected); }
        if (name == "integrity_flags") sawIntegrity = true;
    }
    EXPECT_TRUE(sawTier) << "no tier annotation on the device span";
    EXPECT_TRUE(sawConf) << "no clock_confidence annotation on the device span";
    EXPECT_FALSE(sawIntegrity) << "a clean span must not be flagged";
    ::remove(path.c_str());
}

// §11.3 — a flagged span still RENDERS. Only the annotation distinguishes it
// from a measurement, which is why it has to be there.
TEST(ProfilerClock, aShearedDeviceSpanCarriesItsIntegrityFlags) {
    auto& j = *clk().jit;
    auto toTrace = reinterpret_cast<int64_t (*)(const CajetaGpuEvent*, int64_t, const char*)>(
        j.lookupRawSymbol("__cajeta_prof_gpu_events_to_trace"));
    ASSERT_NE(toTrace, nullptr);

    constexpr int32_t CPU = 3;
    const int64_t launch = 1000000000LL;
    const int64_t shear = 5680000000LL;              // §6.5's measured domain gap
    CajetaGpuEvent evs[1] = {
        gpuEvent(CPU, launch, launch + 8000000LL, launch + shear,
                 launch + shear + 6000000LL, CAJETA_PROF_TIER_DEVICE)};
    const std::string path = tracePath("sheared");
    ASSERT_GT(toTrace(evs, 1, path.c_str()), 0);

    auto facts = readTrace(path);
    bool sawIntegrity = false;
    for (auto& [name, val] : facts.annotations) {
        if (name == "integrity_flags") {
            sawIntegrity = true;
            EXPECT_TRUE(val & CAJETA_SPAN_OUTSIDE_HOST);
        }
    }
    EXPECT_TRUE(sawIntegrity) << "a span outside its own host window was not flagged";
    ::remove(path.c_str());
}

// --- 9.2.d, the other half: §7.8's run-level calibration record ------------
//
// §7.8 asks for four things: driver identity, active layers, calibration
// quality, and which tier produced each measurement. The last is per-span and
// already covered above. The other three are properties of the RUN, and
// without them a reader can see that a timeline was produced but not whether
// it was produced well — a 12-confidence fit and a 99-confidence fit render
// identically.
TEST(ProfilerClock, theRunRecordRecordsCalibrationQualityAndDriverIdentity) {
    auto& j = *clk().jit;
    auto calibrateFn = reinterpret_cast<int32_t (*)(int32_t, ClockReadFn, void*, int32_t, int32_t)>(
        j.lookupRawSymbol("__cajeta_prof_clock_calibrate"));
    auto setDriver = reinterpret_cast<int32_t (*)(int32_t, const char*, const char*)>(
        j.lookupRawSymbol("__cajeta_prof_set_driver_identity"));
    auto toTrace = reinterpret_cast<int64_t (*)(const CajetaGpuEvent*, int64_t, const char*)>(
        j.lookupRawSymbol("__cajeta_prof_gpu_events_to_trace"));
    auto snapClear = reinterpret_cast<int32_t (*)()>(
        j.lookupRawSymbol("__cajeta_prof_clock_snapshot_clear"));
    ASSERT_NE(setDriver, nullptr) << "__cajeta_prof_set_driver_identity not linked";
    ASSERT_NE(calibrateFn, nullptr);
    ASSERT_NE(toTrace, nullptr);

    constexpr int32_t CPU = 3;
    snapClear();
    resetAllDomains();
    ASSERT_EQ(clk().set_period(CPU, 2.5), 1);
    ASSERT_EQ(clk().set_dispersion_cap(50000), 1);
    ASSERT_EQ(setDriver(CPU, "cpu-emulation 0.1", "none"), 1);

    FlakyReader r;
    r.goodAfter = 2;                                  // two rejects, then good
    ASSERT_EQ(calibrateFn(CPU, &FlakyReader::fn, &r, 8, 40), 8);
    const int32_t confidence = clk().confidence(CPU);
    ASSERT_GT(confidence, 0);

    const int64_t launch = 1000000000LL;
    CajetaGpuEvent evs[1] = {
        gpuEvent(CPU, launch, launch + 8000000LL, launch + 1000, launch + 6000000LL,
                 CAJETA_PROF_TIER_DEVICE)};
    const std::string path = tracePath("runmeta");
    ASSERT_GT(toTrace(evs, 1, path.c_str()), 0);

    auto facts = readTrace(path);
    auto findInt = [&](const std::string& k, int64_t* out) {
        for (auto& [n, v] : facts.annotations) if (n == k) { *out = v; return true; }
        return false;
    };
    auto findStr = [&](const std::string& k, std::string* out) {
        for (auto& [n, v] : facts.strings) if (n == k) { *out = v; return true; }
        return false;
    };

    int64_t domains = -1;
    EXPECT_TRUE(findInt("clock_domains_calibrated", &domains));
    EXPECT_EQ(domains, 1);

    int64_t conf = -1;
    EXPECT_TRUE(findInt("clock3_confidence", &conf)) << "no calibration quality in the run record";
    EXPECT_EQ(conf, confidence);

    // Drift as integer milli-ppm: the wire carries no floats, and rounding a
    // -15 ppm device to 0 would erase exactly the term §6.6 is about.
    int64_t driftMilli = 0;
    EXPECT_TRUE(findInt("clock3_drift_ppm_milli", &driftMilli));
    EXPECT_NEAR((double) driftMilli, 40000.0, 1500.0);

    int64_t accepted = 0, rejected = 0, gens = 0;
    EXPECT_TRUE(findInt("clock3_samples", &accepted));
    EXPECT_EQ(accepted, 8);
    EXPECT_TRUE(findInt("clock3_rejected", &rejected));
    EXPECT_EQ(rejected, 2) << "the rejects are part of the quality story";
    EXPECT_TRUE(findInt("clock3_recalibrations", &gens));
    EXPECT_EQ(gens, 1);

    std::string driver, layers;
    EXPECT_TRUE(findStr("clock3_driver", &driver)) << "no driver identity (spec 7.8)";
    EXPECT_EQ(driver, "cpu-emulation 0.1");
    EXPECT_TRUE(findStr("clock3_layers", &layers)) << "no active layers (spec 7.8)";
    EXPECT_EQ(layers, "none");

    ::remove(path.c_str());
}

// An uncalibrated run must not invent a quality figure. Zero domains, no
// per-domain keys — a reader asking "how good was the correlation" gets
// "there wasn't one" rather than a number.
TEST(ProfilerClock, anUncalibratedRunRecordsNoCalibrationQuality) {
    auto& j = *clk().jit;
    auto toTrace = reinterpret_cast<int64_t (*)(const CajetaGpuEvent*, int64_t, const char*)>(
        j.lookupRawSymbol("__cajeta_prof_gpu_events_to_trace"));
    auto snapClear = reinterpret_cast<int32_t (*)()>(
        j.lookupRawSymbol("__cajeta_prof_clock_snapshot_clear"));
    ASSERT_NE(toTrace, nullptr);

    constexpr int32_t CPU = 3;
    snapClear();
    resetAllDomains();                                // clears driver identity too

    const int64_t launch = 1000000000LL;
    CajetaGpuEvent evs[1] = {
        gpuEvent(CPU, launch, launch + 8000000LL, launch + 1000, launch + 6000000LL,
                 CAJETA_PROF_TIER_HOST)};
    const std::string path = tracePath("nocal");
    ASSERT_GT(toTrace(evs, 1, path.c_str()), 0);

    auto facts = readTrace(path);
    int64_t domains = -1;
    for (auto& [n, v] : facts.annotations) if (n == "clock_domains_calibrated") domains = v;
    EXPECT_EQ(domains, 0);
    for (auto& [n, v] : facts.annotations) {
        (void) v;
        EXPECT_EQ(n.rfind("clock3_", 0), std::string::npos)
            << "uncalibrated domain still emitted " << n;
    }
    ::remove(path.c_str());
}
