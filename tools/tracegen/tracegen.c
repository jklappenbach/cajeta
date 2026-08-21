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
#include <string.h>

#include "../../runtime/native/cajeta_rt_prof_trace.c"

int main(int argc, char** argv) {
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
