// cajeta-profiler 5.1.f — generate a minimal .pftrace and let trace_processor
// judge it.
//
// Standalone by design: it textually includes the runtime's trace writer and
// nothing else, so CI validates the ACTUAL emitter with a plain `cc` and no
// LLVM, no compiler build, and no GPU. The alternative — building the whole
// toolchain to produce one trace — would make this check expensive enough that
// it would not run.
//
// What it proves that a unit test cannot: that the bytes load. Round-trip tests
// verify the wire layer against itself, and the vendored proto verifies the
// field NUMBERS, but only a real reader can say whether the resulting file is a
// trace. A structurally wrong trace is valid protobuf with correct field
// numbers, so every local check passes and the artifact is still useless.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Excludes only the ring drain, which reads the sampler's globals. Everything
// the trace actually depends on — the wire layer, the writer, the interning
// table, and the sample->slice transform — is the runtime's own code, compiled
// here verbatim.
#define CAJETA_PROF_TRACE_STANDALONE 1
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include "../../runtime/native/cajeta_prof_abi.h"
// Unit 9's correlation and integrity modules, compiled verbatim like the trace
// writer beside them — the point of this tool is that CI validates the
// runtime's OWN code, so a stub here would validate the stub.
#include "../../runtime/native/cajeta_rt_prof_clock.c"
#include "../../runtime/native/cajeta_rt_prof_integrity.c"
// Unit 10: the exact-instrumentation counters + their run-record annotations.
// Its probe pair needs the runtime's per-fiber shadow stack and is excluded
// from this build; everything the TRACE sees is compiled verbatim.
#include "../../runtime/native/cajeta_rt_prof_instr.c"
#include "../../runtime/native/cajeta_rt_prof_trace.c"

// No fiber-scheduler stub is needed any more: the transform reads the fiber's
// display id out of the SAMPLE (captured when the sample was taken) rather than
// dereferencing the handle at drain time, so nothing here has to fake the
// scheduler.

// A synthetic profile: two tracks, a stack that grows and shrinks the way a
// sampler actually observes one. Drives __cajeta_prof_samples_to_trace — the
// runtime's real transform — so CI judges the code that ships.
//
// The shape matters. Samples 0-2 sit in run->middle->inner, sample 3 leaves
// `inner`, sample 4 leaves `middle` too. A correct diff closes exactly what
// vanished and leaves `run` open throughout; a transform that closed everything
// each tick would still produce a loadable trace, just a wrong one.
// §3.4 — the two tiers in ONE trace. Registering the instrumentation
// descriptors before the sample transform runs is exactly what an instrumented
// build that also armed the sampler looks like at drain time, and it is the
// only way to check that a consumer can still tell the two apart: sampling is
// nested slices on per-owner tracks, instrumentation is instants on
// `cajeta.instrumentation`, and each instant says `source` outright.
static void instr_register_synthetic(void);

static int profile_mode_impl(const char* out, int with_instr);

static int profile_mode(const char* out) { return profile_mode_impl(out, 0); }
static int both_mode(const char* out)    { return profile_mode_impl(out, 1); }

static int profile_mode_impl(const char* out, int with_instr) {
    static CajetaFrameDesc run    = { "test.App", "run",    "App.cajeta" };
    static CajetaFrameDesc middle = { "test.App", "middle", "App.cajeta" };
    static CajetaFrameDesc inner  = { "test.App", "inner",  "App.cajeta" };
    const CajetaFrameDesc* stacks[5][3] = {
        { &inner, &middle, &run },   // innermost-first, as the sampler stores it
        { &inner, &middle, &run },
        { &inner, &middle, &run },
        { &middle, &run, 0 },
        { &run, 0, 0 },
    };
    const int32_t depths[5] = { 3, 3, 3, 2, 1 };

    static CajetaProfSample samples[10];
    int64_t n = 0;
    void* thread_owner = (void*) 0x1000;
    void* fiber_owner  = (void*) 0x2000;
    for (int i = 0; i < 5; i++) {
        CajetaProfSample* s = &samples[n++];
        s->host_ns = 1000000 + (int64_t) i * 500000;
        s->owner = thread_owner;
        s->owner_kind = CAJETA_PROF_OWNER_THREAD;
        s->n_frames = depths[i];
        s->truncated = 0;
        for (int32_t k = 0; k < depths[i]; k++) {
            s->frames[k].desc = stacks[i][k];
            s->frames[k].line = 10 + k;
        }
        // A fiber sampled at the same instants, so §4.3's "fibers are tracks
        // distinct from their carrier" has something to show.
        CajetaProfSample* f = &samples[n++];
        *f = *s;
        f->owner = fiber_owner;
        f->owner_kind = CAJETA_PROF_OWNER_FIBER;
        f->owner_id = 8192;
        f->n_frames = 1;
        f->frames[0].desc = &run;
    }

    // 6.2.c: stamp what produced this run, including what it LOST. A trace
    // whose ring overflowed must not look identical to a complete one.
    CajProfMeta meta;
    meta.tier = "sampling";
    meta.rate_hz = 2000;
    meta.ring_cap = 4096;
    meta.samples = 40;
    meta.dropped = 8;          // deliberately non-zero: CI asserts it survives
    meta.frames = 96;
    if (with_instr) instr_register_synthetic();
    int64_t packets = __cajeta_prof_samples_to_trace_meta(samples, n, out, &meta);
    printf("tracegen: profile mode wrote %lld packets to %s\n",
           (long long) packets, out);
    if (packets <= 0) { fprintf(stderr, "tracegen: transform wrote nothing\n"); return 4; }
    return 0;
}

// A synthetic GPU run: two kernels on one device, on two queues, launched from
// two host threads. Drives __cajeta_prof_gpu_emit — the runtime's real
// transform — so trace_processor judges the device/context/queue hierarchy
// (§7.2) and the launch->execution flow (§7.3) that ship.
//
// Only a real reader can settle these. A flow whose two ends do not agree on an
// id, or a queue track parented to nothing, produces a file that is valid
// protobuf, loads without complaint, and shows no arrows and no hierarchy.
// Unit 9 — a calibration the trace can be checked against. Without one the
// generated trace carries converted device timestamps and no ClockSnapshot, so
// CI could not tell a correct §7.5 emission from a missing one.
static int caj_tracegen_clock_read(int64_t* before, int64_t* ticks,
                                   int64_t* after, void* user) {
    int* i = (int*) user;
    int64_t mid = 1000000 + (int64_t) (*i) * 500000;
    (*i)++;
    *before = mid - 100;
    *ticks  = mid / 2;              // a 2 ns/tick device, exactly in phase
    *after  = mid + 100;
    return 1;
}

static int gpu_mode(const char* out) {
    static CajetaFrameDesc site = { "test.App", "dispatchAll", "App.cajeta" };
    static CajetaGpuEvent evs[4];
    const char* names[4] = { "test.K.saxpy", "test.K.reduce",
                             "test.K.saxpy", "test.K.transpose" };
    for (int i = 0; i < 4; i++) {
        CajetaGpuEvent* e = &evs[i];
        memset(e, 0, sizeof(*e));
        e->launch_id = 100 + i;
        e->host_launch_ns = 1000000 + (int64_t) i * 200000;
        e->dev_start_ns   = e->host_launch_ns + 5000;
        e->dev_end_ns     = e->dev_start_ns + 120000;
        e->host_return_ns = e->dev_end_ns + 2000;
        e->kernel_name = names[i];
        e->call_site = &site;
        e->call_site_line = 30 + i;
        e->backend = 3;                       // cpu emulation
        e->device_id = 0;
        e->tier = CAJETA_PROF_TIER_HOST;
        e->queue = i % 2;                     // two queues under one context
        e->host_thread = (void*) (uintptr_t) (0x3000 + (i % 2) * 0x80);
        e->grid_x = 64; e->grid_y = 1; e->grid_z = 1;
        e->block_x = 32; e->block_y = 1; e->block_z = 1;
    }
    // Calibrate the CPU-emulation domain so the trace carries the ClockSnapshot
    // (§7.5) and the per-measurement confidence (§10.6) alongside the spans.
    int reads = 0;
    __cajeta_prof_clock_snapshot_clear();
    __cajeta_prof_clock_reset(3);
    __cajeta_prof_clock_set_period(3, 2.0);
    // §7.8 — the driver identity a real backend would register at init.
    __cajeta_prof_set_driver_identity(3, "cpu-emulation 0.1", "none");
    __cajeta_prof_clock_calibrate(3, &caj_tracegen_clock_read, &reads, 8, 24);

    int64_t packets = __cajeta_prof_gpu_events_to_trace(evs, 4, out);
    printf("tracegen: gpu mode wrote %lld packets to %s\n",
           (long long) packets, out);
    if (packets <= 0) { fprintf(stderr, "tracegen: gpu transform wrote nothing\n"); return 5; }
    return 0;
}

// Unit 10 — a synthetic INSTRUMENTED run: three methods with known counts, a
// known probe cost, and a selection that omits something. The descriptors are
// built here exactly as ProfileCodegen emits them and registered without a
// probe, because the probe pair needs the runtime's per-fiber shadow stack and
// this build has none. Everything downstream of the descriptor — the run
// record's §3.5/§3.12/§3.13 annotations and the per-method §3.4 track — is the
// shipping code, which is the only way trace_processor gets to judge it.
static void instr_register_synthetic(void) {
    static CajetaProfMethod methods[3] = {
        { "test.App", "run",  "App.cajeta", 1,   4200000, 1, 0, 0, NULL },
        { "test.App", "mid",  "App.cajeta", 40,  3900000, 0, 0, 0, NULL },
        { "test.App", "leaf", "App.cajeta", 400, 1200000, 0, 0, 0, NULL },
    };
    for (int i = 0; i < 3; i++) __cajeta_prof_instr_add(&methods[i]);
    __cajeta_prof_instr_register_build("include test.**; exclude test.Noisy", 2);
    __cajeta_prof_instr_set_probe_pairs(441);
    __cajeta_prof_instr_set_probe_ns(18);
}

static int instr_mode(const char* out) {
    instr_register_synthetic();
    int64_t packets = __cajeta_prof_instr_only_to_trace(out);
    printf("tracegen: instr mode wrote %lld packets to %s\n",
           (long long) packets, out);
    if (packets <= 0) { fprintf(stderr, "tracegen: instr transform wrote nothing\n"); return 6; }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && strcmp(argv[1], "--profile") == 0) return profile_mode(argv[2]);
    if (argc > 2 && strcmp(argv[1], "--gpu") == 0) return gpu_mode(argv[2]);
    if (argc > 2 && strcmp(argv[1], "--instr") == 0) return instr_mode(argv[2]);
    if (argc > 2 && strcmp(argv[1], "--both") == 0) return both_mode(argv[2]);
    const char* out = (argc > 1) ? argv[1] : "cajeta-min.pftrace";
    static CajProfWriter w;
    if (!__cajeta_prof_trace_open(&w, out)) {
        fprintf(stderr, "tracegen: cannot open %s\n", out);
        return 1;
    }

    const uint64_t thread_track = 0x1000;
    const uint64_t child_track  = 0x1001;

    // A track, and a child of it — 5.1.e's hierarchy.
    __cajeta_prof_trace_track(&w, thread_track, 0, "cajeta.main");
    __cajeta_prof_trace_track(&w, child_track, thread_track, "cajeta.fiber.1");

    // Nested and overlapping slices — 5.1.d. Outer encloses inner on one track;
    // the child track carries an independent slice spanning the boundary. Names
    // are interned by the writer: "test.App.run" is passed three times and must
    // be emitted once (5.1.c), with the first InternedData packet carrying
    // CLEARED and every consuming slice carrying NEEDS.
    __cajeta_prof_trace_slice(&w, 1000, thread_track, CAJ_TE_SLICE_BEGIN, "test.App.run");
    __cajeta_prof_trace_slice(&w, 1200, child_track,  CAJ_TE_SLICE_BEGIN, "test.App.run");
    __cajeta_prof_trace_slice(&w, 1500, thread_track, CAJ_TE_SLICE_BEGIN, "test.App.run");
    __cajeta_prof_trace_slice(&w, 1800, thread_track, CAJ_TE_SLICE_END,   NULL);
    __cajeta_prof_trace_slice(&w, 2200, child_track,  CAJ_TE_SLICE_END,   NULL);
    __cajeta_prof_trace_slice(&w, 2500, thread_track, CAJ_TE_SLICE_END,   NULL);

    int32_t interned = __cajeta_prof_trace_interned(&w);
    int64_t packets = __cajeta_prof_trace_packets(&w);
    int64_t bytes = __cajeta_prof_trace_bytes(&w);
    __cajeta_prof_trace_close(&w);

    printf("tracegen: wrote %lld bytes, %lld packets, %d interned name(s) to %s\n",
           (long long) bytes, (long long) packets, interned, out);
    // One name passed three times must intern once, or 5.1.c is not met.
    if (interned != 1) {
        fprintf(stderr, "tracegen: expected 1 interned name, got %d\n", interned);
        return 3;
    }
    return 0;
}
