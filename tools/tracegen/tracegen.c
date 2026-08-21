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
    static uint8_t buf[1 << 16];
    CajPbBuf b = { buf, (int32_t) sizeof(buf), 0, 0 };

    const uint32_t seq = 1;
    const uint64_t thread_track = 0x1000;
    const uint64_t child_track  = 0x1001;

    // A track, and a child of it — 5.1.e's hierarchy.
    __cajeta_prof_emit_track(&b, thread_track, 0, "cajeta.main");
    __cajeta_prof_emit_track(&b, child_track, thread_track, "cajeta.fiber.1");

    // One interned name, referenced by both slices below — 5.1.c.
    __cajeta_prof_emit_name(&b, seq, 1, "test.App.run");

    // Nested and overlapping slices — 5.1.d. Outer encloses inner on one track;
    // the child track carries an independent slice that spans the boundary.
    __cajeta_prof_emit_slice(&b, seq, 1000, thread_track, CAJ_TE_SLICE_BEGIN, 1, 0, 1);
    __cajeta_prof_emit_slice(&b, seq, 1200, child_track,  CAJ_TE_SLICE_BEGIN, 1, 0, 0);
    __cajeta_prof_emit_slice(&b, seq, 1500, thread_track, CAJ_TE_SLICE_BEGIN, 1, 0, 0);
    __cajeta_prof_emit_slice(&b, seq, 1800, thread_track, CAJ_TE_SLICE_END,   0, 0, 0);
    __cajeta_prof_emit_slice(&b, seq, 2200, child_track,  CAJ_TE_SLICE_END,   0, 0, 0);
    __cajeta_prof_emit_slice(&b, seq, 2500, thread_track, CAJ_TE_SLICE_END,   0, 0, 0);

    if (b.overflow) {
        fprintf(stderr, "tracegen: buffer overflow, trace is short\n");
        return 2;
    }
    FILE* f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "tracegen: cannot open %s\n", out); return 1; }
    fwrite(buf, 1, (size_t) b.len, f);
    fclose(f);
    printf("tracegen: wrote %d bytes to %s\n", b.len, out);
    return 0;
}
