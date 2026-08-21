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

// ── GPU dispatch records (Unit 7, spec §5.1, §5.3, §5.6) ──────────────────
//
// The seam between "a kernel ran" and "somebody wants to know". Deliberately a
// RECORD, not a vendor handle: adding a backend must not touch the writer or
// the UI, and adding a consumer must not touch a backend (plan 7.3.a, 7.3.b).

// Which mechanism produced the device times (spec §5.1.4, §7.8). The tier names
// the MECHANISM, not the accuracy — a consumer weights by it, so it must never
// be upgraded on a hunch. TIER_HOST on the CPU-emulation backend is exact by
// construction (the dispatch is synchronous, so host wall time IS the kernel's
// span), yet it is still reported as host-side, because a consumer cannot tell
// "exact because there is no device" from "degraded because the device would
// not say" unless the mechanism is what gets reported.
#define CAJETA_PROF_TIER_DEVICE 0   // vendor profiler dispatch records
#define CAJETA_PROF_TIER_EVENT  1   // device event bracketing
#define CAJETA_PROF_TIER_HOST   2   // host submit-to-complete

// Delivery granularity a sink asks for at registration (spec §5.6.8, §14.12).
// Per-sink, never global: a global per-record rule taxes the writer, which is
// filling a file and does not care; a global batched rule puts an invisible
// floor under every consumer's reaction time.
#define CAJETA_GPU_SINK_BATCHED    0   // the default an undeclared sink gets
#define CAJETA_GPU_SINK_PER_RECORD 1

#define CAJETA_GPU_MAX_SINKS       8
#define CAJETA_GPU_SINK_QUEUE      1024   // per sink; power of two (see the ring)

typedef struct {
    int64_t     launch_id;       // unique + monotonic across the process
    int64_t     host_launch_ns;  // host clock, at the seam's entry
    int64_t     host_return_ns;  // host clock, after the backend returned
    int64_t     dev_start_ns;    // ALREADY host-domain (spec §5.1.7)
    int64_t     dev_end_ns;
    const char* kernel_name;     // module constant data; program lifetime
    // The host call site that launched it (spec §5.1.2). Read from the same
    // line-info shadow stack the sampler reads, so a launch and a sample agree
    // about where the program was — and so the flow arrow lands on a real
    // file:line rather than on a synthetic marker.
    const CajetaFrameDesc* call_site;
    int32_t     call_site_line;
    int32_t     backend;         // CAJ_XPU_* (an int here: the record must not
                                 // drag the XPU ABI into every consumer)
    int32_t     device_id;
    int32_t     tier;            // CAJETA_PROF_TIER_*
    int64_t     queue;           // stream/queue handle; 0 = default
    void*       host_thread;     // launching thread handle (track identity)
    int32_t     grid_x, grid_y, grid_z;
    int32_t     block_x, block_y, block_z;
    uint32_t    shared_bytes;
} CajetaGpuEvent;

// A sink returns 0 for "handled" and non-zero for "I faulted". A crash inside a
// callback is not something this seam can catch portably, so "fault" means the
// sink SAYS it failed — which is the only failure a C ABI can carry honestly.
// A faulting sink is disabled and reported (spec §5.6.5).
typedef int32_t (*CajetaGpuSinkFn)(const CajetaGpuEvent* recs, int32_t n,
                                   void* user);

// Per-backend timing vtable (plan 7.2.a). `collect` drains whatever the vendor
// buffered; `calibrate` re-establishes the device->host mapping (§6). The CPU
// backend implements the first two and no-ops the rest.
typedef struct {
    const char* name;
    int32_t (*init)(void);
    int32_t (*begin_launch)(CajetaGpuEvent* ev);
    int32_t (*end_launch)(CajetaGpuEvent* ev);
    int32_t (*collect)(void);
    int32_t (*calibrate)(void);
} CajetaGpuBackendVtbl;

#endif
