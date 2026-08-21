// cajeta-profiler — the shapes the sampler produces and the trace writer
// consumes. Single source of truth, in the spirit of cajeta_xpu_abi.h.
//
// Split out 2026-08-21 so the sample->slice transform can be driven by a
// caller-supplied array rather than only by the sampler's ring. That is what
// lets tools/tracegen build synthetic samples and run the REAL transform under
// a plain `cc` in CI — no LLVM, no toolchain build — which is the only way the
// resulting trace gets judged by trace_processor on every change.
#ifndef CAJETA_PROF_ABI_H
#define CAJETA_PROF_ABI_H

#include <stdint.h>

#define CAJETA_SHADOW_MAX 512
#define CAJETA_PROF_MAX_FRAMES 128
#define CAJETA_PROF_OWNER_THREAD 0
#define CAJETA_PROF_OWNER_FIBER  1

// Codegen-emitted, program lifetime. Matches the #FrameDesc constant
// LineInfoCodegen builds at each method prologue.
typedef struct {
    const char* typeName;    // "test.App"
    const char* methodName;  // "run"
    const char* fileName;    // "App.cajeta"
} CajetaFrameDesc;

typedef struct {
    const CajetaFrameDesc* desc;
    int32_t line;
} CajetaShadowFrame;

typedef struct {
    int64_t          host_ns;       // when the sample was taken
    void*            owner;         // thread or fiber handle it came from
    int32_t          owner_kind;    // THREAD or FIBER — the sampler knows which
                                    // registry it read, and the handle alone
                                    // cannot say, so Unit 6 would otherwise have
                                    // to guess when naming tracks (spec §4.3
                                    // wants fibers distinct from carriers).
    int32_t          n_frames;      // innermost-first, as snapshot returns them
    int32_t          truncated;     // source stack was deeper than capacity
    CajetaShadowFrame frames[CAJETA_PROF_MAX_FRAMES];
} CajetaProfSample;

#endif
