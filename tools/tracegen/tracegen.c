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
#include "../../runtime/native/cajeta_prof_abi.h"
#include "../../runtime/native/cajeta_rt_prof_trace.c"

// The transform names a fiber track by its debugger id, which lives in the
// fiber scheduler — not compiled here. A stub keeps the standalone build honest:
// the id is cosmetic (it appears in the track NAME), so substituting a
// deterministic one changes what the track is called and nothing about the
// structure CI checks.
long __cajeta_dbg_fiber_id_of(void* fiber) { return (long) (uintptr_t) fiber; }

// A synthetic profile: two tracks, a stack that grows and shrinks the way a
// sampler actually observes one. Drives __cajeta_prof_samples_to_trace — the
// runtime's real transform — so CI judges the code that ships.
//
// The shape matters. Samples 0-2 sit in run->middle->inner, sample 3 leaves
// `inner`, sample 4 leaves `middle` too. A correct diff closes exactly what
// vanished and leaves `run` open throughout; a transform that closed everything
// each tick would still produce a loadable trace, just a wrong one.
static int profile_mode(const char* out) {
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
        f->n_frames = 1;
        f->frames[0].desc = &run;
    }

    int64_t packets = __cajeta_prof_samples_to_trace(samples, n, out);
    printf("tracegen: profile mode wrote %lld packets to %s\n",
           (long long) packets, out);
    if (packets <= 0) { fprintf(stderr, "tracegen: transform wrote nothing\n"); return 4; }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && strcmp(argv[1], "--profile") == 0) return profile_mode(argv[2]);
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
