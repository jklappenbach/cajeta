// cajeta-profiler — the shapes the sampler produces and the trace writer consumes.
// Split out so the sample->slice transform runs on a caller-supplied array under `cc`.
#ifndef CAJETA_PROF_ABI_H
#define CAJETA_PROF_ABI_H

#include <stdint.h>

#define CAJETA_SHADOW_MAX 512
#define CAJETA_PROF_MAX_FRAMES 128
#define CAJETA_PROF_OWNER_THREAD 0
#define CAJETA_PROF_OWNER_FIBER  1

// Codegen-emitted, program lifetime: the #FrameDesc LineInfoCodegen builds per prologue.
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
    int32_t          owner_kind;    // THREAD or FIBER; the handle alone cannot say
    // The owner's DISPLAY id, read AT SAMPLE TIME, the only moment the handle is live.
    int64_t          owner_id;
    int32_t          n_frames;      // innermost-first, as snapshot returns them
    int32_t          truncated;     // source stack was deeper than capacity
    CajetaShadowFrame frames[CAJETA_PROF_MAX_FRAMES];
} CajetaProfSample;

// ── GPU dispatch records (Unit 7, spec §5.1, §5.3, §5.6) ──────────────────
// A RECORD, not a vendor handle, so a new backend touches no consumer and vice versa.

// Which mechanism produced the device times: the MECHANISM, never the accuracy, so it
// must not be upgraded on a hunch. ZERO IS UNKNOWN because events are minted by memset.
#define CAJETA_PROF_TIER_UNKNOWN 0  // never assigned by any backend
#define CAJETA_PROF_TIER_DEVICE  1  // vendor profiler dispatch records
#define CAJETA_PROF_TIER_EVENT   2  // device event bracketing
#define CAJETA_PROF_TIER_HOST    3  // host submit-to-complete

// Delivery granularity, per-sink: a global rule taxes the writer or floors a consumer.
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
    // The host call site, from the SAME shadow stack the sampler reads.
    const CajetaFrameDesc* call_site;
    int32_t     call_site_line;
    int32_t     backend;         // CAJ_XPU_*, an int so consumers need no XPU ABI
    int32_t     device_id;
    int32_t     tier;            // CAJETA_PROF_TIER_*
    int64_t     queue;           // stream/queue handle; 0 = default
    void*       host_thread;     // launching thread handle (track identity)
    int32_t     grid_x, grid_y, grid_z;
    int32_t     block_x, block_y, block_z;
    uint32_t    shared_bytes;
    // Host clock when a vendor record claimed this launch; 0 = none did. It closes the
    // bracket a device span must lie inside, and it is the proof of a TIER_DEVICE claim.
    int64_t     resolved_ns;
    // §11.3 flags only the PRODUCER can know, OR'd with the checker's own at emit.
    int32_t     integrity_flags;
} CajetaGpuEvent;

// A sink returns 0 for "handled", non-zero for "I faulted"; a faulting sink is disabled.
typedef int32_t (*CajetaGpuSinkFn)(const CajetaGpuEvent* recs, int32_t n,
                                   void* user);

// Per-backend timing vtable. `collect` drains whatever the vendor buffered;
// `calibrate` re-establishes the device->host mapping. The CPU backend no-ops most.
typedef struct {
    const char* name;
    int32_t (*init)(void);
    int32_t (*begin_launch)(CajetaGpuEvent* ev);
    int32_t (*end_launch)(CajetaGpuEvent* ev);
    int32_t (*collect)(void);
    int32_t (*calibrate)(void);
} CajetaGpuBackendVtbl;

// Backend ids in CajetaGpuEvent.backend; must agree with caj_gpu_backend_name's switch.
#define CAJ_GPU_BACKEND_CUDA    0
#define CAJ_GPU_BACKEND_HIP     1
#define CAJ_GPU_BACKEND_VULKAN  2
#define CAJ_GPU_BACKEND_CPU     3

// ── Unit 8: ROCm backend binding state (spec §5.2) ────────────────────────
// A STATE, not a bool: absent, configured too late, and bound-but-silent differ.
#define CAJETA_ROCM_UNATTEMPTED 0   // init() has not run
#define CAJETA_ROCM_ABSENT      1   // no librocprofiler-sdk to bind (§5.2.2)
#define CAJETA_ROCM_READY       2   // bound and tracing
#define CAJETA_ROCM_LATE        3   // configure attempted after HIP init (§5.2.3)
#define CAJETA_ROCM_NO_RECORDS  4   // zero records past the threshold (§5.2.5)

// Bind rocprofiler-sdk: 1 when READY. The state and reason are readable either way.
int32_t     __cajeta_prof_rocm_init(void);
void        __cajeta_prof_rocm_reset(void);
int32_t     __cajeta_prof_rocm_state(void);
const char* __cajeta_prof_rocm_reason(void);
const char* __cajeta_prof_rocm_lib_path(void);
// Entry points needed vs. bound: wrong library and wrong SDK version look different.
int32_t     __cajeta_prof_rocm_entry_count(void);
int32_t     __cajeta_prof_rocm_entries_bound(void);
// Configure rocprofiler; must run BEFORE HIP finishes init, or the SDK refuses (LATE).
int32_t     __cajeta_prof_rocm_configure(void);
int32_t     __cajeta_prof_rocm_configured(void);
int32_t     __cajeta_prof_rocm_tool_init_ran(void);

// ── Unit 8.2.c: buffered kernel-dispatch tracing ──────────────────────────
// The device's answer arrives LATER than its launch, so the launch id rides along as
// the SDK's correlation id. Flushing per launch would serialize the streams.
int32_t     __cajeta_prof_rocm_push(int64_t launchId);
int32_t     __cajeta_prof_rocm_pop(void);
int32_t     __cajeta_prof_rocm_flush(void);
int32_t     __cajeta_prof_rocm_tracing(void);
int32_t     __cajeta_prof_rocm_dispatch_kind(void);
int64_t     __cajeta_prof_rocm_records(void);
int64_t     __cajeta_prof_rocm_unmatched(void);
int64_t     __cajeta_prof_rocm_clock_offset_ns(void);
int64_t     __cajeta_prof_rocm_device_now_ns(void);
int64_t     __cajeta_prof_rocm_launches(void);
int32_t     __cajeta_prof_rocm_record_threshold(void);
// BOOTTIME minus MONOTONIC jumps by the sleep duration on suspend, leaving a trace that
// renders perfectly yet minutes out of place. Split from sampling so it stays testable.
int32_t     __cajeta_prof_rocm_note_clock_offset(int64_t offsetNs);
int32_t     __cajeta_prof_rocm_suspended(void);
int64_t     __cajeta_prof_rocm_suspend_ns(void);

// Hand a device record (already in the host clock domain) back to the launch waiting
// for it; 1 if one claimed it. Declared here: ROCm compiles BEFORE the seam in one TU.
int32_t     __cajeta_prof_gpu_resolve_dispatch(int64_t launchId,
                                               int64_t devStartNs, int64_t devEndNs);
// The tier-explicit form: the caller names the MECHANISM that supplied the span.
int32_t     __cajeta_prof_gpu_resolve_dispatch_tier(int64_t launchId,
                                                    int64_t devStartNs,
                                                    int64_t devEndNs,
                                                    int32_t tier);
// The full form: producer-known §11.3 flags ride the event to the checker.
int32_t     __cajeta_prof_gpu_resolve_dispatch_flags(int64_t launchId,
                                                     int64_t devStartNs,
                                                     int64_t devEndNs,
                                                     int32_t tier,
                                                     int32_t integrityFlags);

// ── Unit 13: the Vulkan backend (spec §5.5, §6.5, §6.6) ───────────────────
// Split so the PURE half compiles with no Vulkan SDK; the API half sits by the dispatcher.
int32_t  __cajeta_xpu_vk_pick_queue_family(const uint32_t* queueFlags,
                                           const uint32_t* timestampValidBits,
                                           int32_t n, int32_t* timingOk);
// ICD preference and the two-way split of "no Vulkan"; pure, so testable off Apple.
int32_t  __cajeta_xpu_vk_pick_device(const uint32_t* driverIds, int32_t n,
                                     const char* force);
int32_t  __cajeta_xpu_vk_classify_init(int32_t loaderFound, int32_t deviceCount);
uint64_t __cajeta_prof_vk_delta_ticks(uint64_t startTicks, uint64_t endTicks,
                                      uint32_t validBits);
int32_t  __cajeta_prof_vk_note_span_ticks(uint64_t startTicks,
                                          uint64_t endTicks);
void     __cajeta_prof_vk_span_tracking_reset(void);
int32_t  __cajeta_prof_vk_configure(uint32_t validBits, double periodNs,
                                    int32_t hasCalibration);
int32_t  __cajeta_prof_vk_timing_ok(void);
uint32_t __cajeta_prof_vk_valid_bits(void);
double   __cajeta_prof_vk_period_ns(void);
int64_t  __cajeta_prof_vk_resets(void);
int64_t  __cajeta_prof_vk_spans(void);
int64_t  __cajeta_prof_vk_unavailable(void);
void     __cajeta_prof_vk_note_resolved(void);
void     __cajeta_prof_vk_note_unavailable(void);
void     __cajeta_prof_vk_reset(void);
// The seam half: the launch id the dispatcher brackets under, and the hand-back.
int64_t  __cajeta_prof_vk_current_launch(void);
void     __cajeta_prof_vk_bracket_resolved(int64_t launchId, int64_t devStartNs,
                                           int64_t devEndNs, int32_t flags);
int32_t  __cajeta_prof_vk_note_wait(int64_t queue, int64_t startNs,
                                    int64_t endNs);

// ── CUDA event-tier fallback: device timing with no CUDA Toolkit ─────────
// CUPTI ships with the Toolkit but cuEventRecord with the DRIVER, so a driver-only
// machine reaches EVENT tier. The DISPATCHER owns the events; the seam, the launch id.
int64_t  __cajeta_prof_cuda_current_launch(void);
void     __cajeta_prof_cuda_bracket_resolved(int64_t launchId, int64_t devStartNs,
                                             int64_t devEndNs);
// Armed once the event entry points and the anchoring reference event exist.
void     __cajeta_prof_cuda_events_note(int32_t ok, const char* why);
int32_t  __cajeta_prof_cuda_events_ok(void);
const char* __cajeta_prof_cuda_events_reason(void);
int64_t  __cajeta_prof_cuda_event_spans(void);

// ── Unit 12: CUPTI binding state (spec §5.4) ──────────────────────────────
// As with ROCm: a missing CUPTI degrades and says so, never reads as a working backend.
#define CAJETA_CUPTI_UNATTEMPTED 0   // init() has not run
#define CAJETA_CUPTI_ABSENT      1   // no libcupti to bind (§5.4.2)
#define CAJETA_CUPTI_READY       2   // core entry points bound

int32_t     __cajeta_prof_cupti_init(void);
void        __cajeta_prof_cupti_reset(void);
int32_t     __cajeta_prof_cupti_state(void);
const char* __cajeta_prof_cupti_reason(void);
const char* __cajeta_prof_cupti_lib_path(void);
int32_t     __cajeta_prof_cupti_entry_count(void);
int32_t     __cajeta_prof_cupti_entries_bound(void);
// Newer than the core Activity API (CUDA 11.6): its ABSENCE selects a conversion path.
int32_t     __cajeta_prof_cupti_has_timestamp_callback(void);
// WSL accepts the timestamp callback then ignores it; the parse is split to stay testable.
int32_t     __cajeta_prof_cupti_version_is_wsl(const char* procVersion);
int32_t     __cajeta_prof_cupti_on_wsl(void);
/* The external-correlation chokepoint; `pushes`/`pops` count ATTEMPTS at entry. */
// Arming, which binding is NOT: registers the buffer callbacks and enables the kinds
// the seam resolves with. Until it runs, every CUDA launch publishes at host tier.
int64_t     __cajeta_prof_cupti_ext_records(void);
int64_t     __cajeta_prof_cupti_unmapped(void);
int32_t     __cajeta_prof_cupti_configure(void);
int32_t     __cajeta_prof_cupti_configured(void);
int32_t     __cajeta_prof_cupti_tracing(void);
int32_t     __cajeta_prof_cupti_push(int64_t launchId);
int32_t     __cajeta_prof_cupti_pop(void);
int64_t     __cajeta_prof_cupti_pushes(void);
int64_t     __cajeta_prof_cupti_pops(void);
int32_t     __cajeta_prof_cupti_flush(void);
// Launches parked for a record, and the two ways one lands at host tier anyway.
int32_t     __cajeta_prof_gpu_collect(int32_t backend);
// ── Unit 6.6: GPU capture, armed by the same environment as the sampler ────
// GPU work lands in the SAME trace as the samples; the ring drops the oldest and counts.
int32_t     __cajeta_prof_gpu_capture_arm(int32_t cap);
void        __cajeta_prof_gpu_capture_disarm(void);
int64_t     __cajeta_prof_gpu_captured(void);
int64_t     __cajeta_prof_gpu_capture_dropped(void);
int32_t     __cajeta_prof_gpu_pending_count(void);
void        __cajeta_prof_gpu_pending_reset(void);
int64_t     __cajeta_prof_gpu_pending_overflow(void);
int64_t     __cajeta_prof_gpu_pending_unclaimed(void);

// ── Unit 9: clock correlation and integrity (spec §6, §11) ────────────────
// A DOMAIN is one device clock mapped onto the host timeline, keyed by CAJ_XPU_* id.
// The synthetic domain has a chosen offset and drift the CPU backend's zeros cannot.
#define CAJETA_CLOCK_MAX_DOMAINS   8
#define CAJETA_CLOCK_DOMAIN_SYNTH  7

// A calibration sample is a sandwich; the gap between its host reads IS its uncertainty.
#define CAJETA_CLOCK_OK                 0
#define CAJETA_CLOCK_REJECT_DISPERSION  1   // sandwich wider than the cap (§6.7)
#define CAJETA_CLOCK_REJECT_PERIOD      2   // no plausible period set (§11.4)
#define CAJETA_CLOCK_REJECT_BACKWARD    3   // host clock went backwards mid-sample
#define CAJETA_CLOCK_REJECT_DOMAIN      4   // domain id out of range

// Span integrity: a bitmask, because a span can be several kinds of wrong at once.
#define CAJETA_SPAN_OK            0
#define CAJETA_SPAN_NONMONOTONIC  (1 << 0)  // started before the previous span did
#define CAJETA_SPAN_NEGATIVE      (1 << 1)  // end precedes start
#define CAJETA_SPAN_IMPLAUSIBLE   (1 << 2)  // duration outside any sane bound
#define CAJETA_SPAN_UNCORRELATED  (1 << 3)  // no trustworthy mapping (§11.6)
#define CAJETA_SPAN_OUTSIDE_HOST  (1 << 4)  // device span escapes its own launch

// Longest a dispatch may plausibly claim: an hour is a broken timestamp, not a kernel.
#define CAJETA_SPAN_MAX_NS  (60LL * 1000000000LL)

// Bounds on a timestamp period, in host ns per tick; the outer ones catch a units flip.
#define CAJETA_CLOCK_PERIOD_MIN  1e-6
#define CAJETA_CLOCK_PERIOD_MAX  1e9

int32_t __cajeta_prof_clock_reset(int32_t domain);
int32_t __cajeta_prof_clock_set_period(int32_t domain, double nsPerTick);
double  __cajeta_prof_clock_period(int32_t domain);
int32_t __cajeta_prof_clock_set_dispersion_cap(int64_t ns);
int64_t __cajeta_prof_clock_dispersion_cap(void);
int32_t __cajeta_prof_clock_sample(int32_t domain, int64_t hostBeforeNs,
                                   int64_t devTicks, int64_t hostAfterNs);
int32_t __cajeta_prof_clock_samples(int32_t domain);
int32_t __cajeta_prof_clock_rejected(int32_t domain);
int32_t __cajeta_prof_clock_valid(int32_t domain);
int64_t __cajeta_prof_clock_to_host(int32_t domain, int64_t devTicks);
double  __cajeta_prof_clock_drift_ppm(int32_t domain);
int64_t __cajeta_prof_clock_offset_ns(int32_t domain);
int32_t __cajeta_prof_clock_confidence(int32_t domain);
int32_t __cajeta_prof_clock_check_span(int32_t domain, int64_t startNs,
                                       int64_t endNs);

// One backend-supplied calibration read: fill the sandwich, return non-zero on success.
typedef int32_t (*CajetaClockReadFn)(int64_t* hostBeforeNs, int64_t* devTicks,
                                     int64_t* hostAfterNs, void* user);

// Sample to `wantSamples` or `maxAttempts`, returning the count accepted. Bounded here
// so no backend's own loop can hang the profiler on a device in a low power state.
int32_t __cajeta_prof_clock_calibrate(int32_t domain, CajetaClockReadFn read,
                                      void* user, int32_t wantSamples,
                                      int32_t maxAttempts);

// ── Tier ladder and demotion (spec §10.4, §11.1, §11.2) ───────────────────
// DEVICE -> EVENT -> HOST, one rung at a time, floored at HOST. The FIRST reason is real.
#define CAJETA_DEMOTE_NONE           0
#define CAJETA_DEMOTE_STARTUP_CHECK  1   // §11.1 self-verification failed
#define CAJETA_DEMOTE_NO_RECORDS     2   // §11.2 accepted launches, delivered none
#define CAJETA_DEMOTE_BAD_PERIOD     3   // §11.4 driver's period was implausible
#define CAJETA_DEMOTE_NO_CLOCK       4   // §11.6 no trustworthy correlation
#define CAJETA_DEMOTE_NODE           5   // §10.2 device node absent or closed

// Launches accepted while delivering nothing before timing is disabled. Small on purpose.
#define CAJETA_TIER_RECORD_THRESHOLD 8

int32_t __cajeta_prof_tier_reset(int32_t domain);
int32_t __cajeta_prof_tier(int32_t domain);
int32_t __cajeta_prof_tier_reason(int32_t domain);
int32_t __cajeta_prof_tier_demote(int32_t domain, int32_t reason);
int32_t __cajeta_prof_tier_set_record_threshold(int32_t launches);
int32_t __cajeta_prof_tier_note_launch(int32_t domain);
int32_t __cajeta_prof_tier_note_records(int32_t domain, int64_t n);
int32_t __cajeta_prof_tier_launches(int32_t domain);
int64_t __cajeta_prof_tier_records(int32_t domain);

// Startup self-verification over real dispatches: sane durations, and consecutive
// dispatches that DIFFER — the stuck-counter check nothing else catches.
int32_t __cajeta_prof_tier_verify(int32_t domain, const int64_t* startsNs,
                                  const int64_t* endsNs, int32_t n);

// ── Undo stack for partial initialization (spec §10.3) ────────────────────
// A failure partway leaves NOTHING enabled; steps unwind in reverse, 3 may need 1.
#define CAJETA_UNDO_MAX_STEPS 16

typedef void (*CajetaUndoFn)(void* user);

int32_t __cajeta_prof_undo_push(int32_t domain, CajetaUndoFn fn, void* user);
int32_t __cajeta_prof_undo_depth(int32_t domain);
int32_t __cajeta_prof_undo_unwind(int32_t domain);
int32_t __cajeta_prof_undo_commit(int32_t domain);

// ── Device node diagnosis (spec §10.2) ────────────────────────────────────
// "Not installed" and "you lack permission" have completely different fixes.
#define CAJETA_NODE_OK            0
#define CAJETA_NODE_ABSENT        1
#define CAJETA_NODE_INACCESSIBLE  2

int32_t __cajeta_prof_probe_node(const char* path);
const char* __cajeta_prof_node_advice(int32_t status, const char* path);

// Cross-check a dispatch record against itself: the device span must sit inside the host
// window that produced it. This is what catches a whole clock domain being wrong.
int32_t __cajeta_prof_check_dispatch(const CajetaGpuEvent* ev);

// ── Clock snapshots (spec §7.5) ───────────────────────────────────────────
// Each calibration records the pair it anchored on, so the mapping is reproducible FROM
// THE TRACE. Perfetto ids [64,127] are SEQUENCE-SCOPED: one sequence, id 64 + domain.
#define CAJETA_CLOCK_PERFETTO_BASE_ID 64
#define CAJETA_CLOCK_MAX_SNAPSHOTS    32

typedef struct {
    int32_t domain;
    int32_t generation;   // which calibration round produced it
    int64_t hostNs;
    int64_t devTicks;
} CajetaClockSnapshot;

// Driver identity and layers from the domain's backend, COPIED: a vendor string may die.
#define CAJETA_DRIVER_ID_MAX 96

int32_t __cajeta_prof_set_driver_identity(int32_t domain, const char* driver,
                                          const char* layers);
const char* __cajeta_prof_driver_identity(int32_t domain);
const char* __cajeta_prof_active_layers(int32_t domain);

int32_t __cajeta_prof_clock_generation(int32_t domain);
int32_t __cajeta_prof_clock_snapshot_count(void);
int32_t __cajeta_prof_clock_snapshot_get(int32_t index, CajetaClockSnapshot* out);
int32_t __cajeta_prof_clock_snapshot_clear(void);

#endif
