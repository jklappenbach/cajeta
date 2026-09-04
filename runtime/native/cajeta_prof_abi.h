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
    // The owner's DISPLAY id, captured when the sample was taken. The transform
    // used to call __cajeta_dbg_fiber_id_of(owner) at drain time, which
    // dereferences the fiber struct — long after a short-lived fiber has been
    // freed. Sampling is the only moment the handle is known live, so the id is
    // read there. Threads carry 0 and are named by registry index.
    int64_t          owner_id;
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
//
// ZERO IS UNKNOWN, on purpose (plan 6.7.2.a). Every CajetaGpuEvent is minted by
// memset, so whatever value 0 names is the claim an event makes when nothing
// assigned its tier — and DEVICE held that slot until 2026-08-24, which made
// the unassigned default the strongest claim a span can make. The §10.4
// demotion ladder walks DEVICE -> EVENT -> HOST by increment and floors at
// HOST; UNKNOWN sits below DEVICE and is never a rung, only the unassigned
// state.
#define CAJETA_PROF_TIER_UNKNOWN 0  // never assigned by any backend
#define CAJETA_PROF_TIER_DEVICE  1  // vendor profiler dispatch records
#define CAJETA_PROF_TIER_EVENT   2  // device event bracketing
#define CAJETA_PROF_TIER_HOST    3  // host submit-to-complete

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
    // Host clock at the moment a vendor record claimed this launch; 0 = no
    // record ever did. Two duties (plan 6.7.2.c): it closes the causal bracket
    // the integrity check bounds a device span by — a real span lies between
    // its submit and its resolution, however asynchronous the dispatch — and
    // it is the proof behind a TIER_DEVICE claim, which without it is a
    // device span no record supplied.
    int64_t     resolved_ns;
    // §11.3 flags only the PRODUCER can know, OR'd with what the checker
    // derives at emit. The first user is §5.5.7: a Vulkan timestamp-register
    // reset is visible only in the backend's own span history — the flagged
    // span itself is internally flawless, and nothing downstream could
    // rediscover the condition.
    int32_t     integrity_flags;
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

// Backend ids carried in CajetaGpuEvent.backend. Named here rather than left
// as the bare integers they were, because Unit 8 gives them a second reader:
// until now only caj_gpu_backend_name()'s switch cared, and a selector that
// disagreed with that switch by one would silently bind the wrong lane.
#define CAJ_GPU_BACKEND_CUDA    0
#define CAJ_GPU_BACKEND_HIP     1
#define CAJ_GPU_BACKEND_VULKAN  2
#define CAJ_GPU_BACKEND_CPU     3

// ── Unit 8: ROCm backend binding state (spec §5.2) ────────────────────────
//
// Why a STATE and not a bool: §5.2.2 requires that a degraded run say so, and
// the four ways this backend can fail to deliver device timing call for
// different responses. "The SDK is not installed" is a deployment fact;
// "configured too late" is a bug in our own init order (§5.2.3); "bound but
// delivered nothing" is a driver or permission problem (§5.2.5). Collapsing
// them into one flag would leave a consumer unable to tell which.
#define CAJETA_ROCM_UNATTEMPTED 0   // init() has not run
#define CAJETA_ROCM_ABSENT      1   // no librocprofiler-sdk to bind (§5.2.2)
#define CAJETA_ROCM_READY       2   // bound and tracing
#define CAJETA_ROCM_LATE        3   // configure attempted after HIP init (§5.2.3)
#define CAJETA_ROCM_NO_RECORDS  4   // zero records past the threshold (§5.2.5)

// Attempt to bind rocprofiler-sdk. Returns 1 when READY, 0 otherwise; the
// state and the human-readable reason are readable afterwards either way.
int32_t     __cajeta_prof_rocm_init(void);
void        __cajeta_prof_rocm_reset(void);
int32_t     __cajeta_prof_rocm_state(void);
const char* __cajeta_prof_rocm_reason(void);
const char* __cajeta_prof_rocm_lib_path(void);
// How many entry points the backend needs, and how many the last attempt
// resolved. Kept separate so "bound nothing" (wrong library) and "bound most"
// (wrong SDK version) are distinguishable without parsing the reason string.
int32_t     __cajeta_prof_rocm_entry_count(void);
int32_t     __cajeta_prof_rocm_entries_bound(void);
// Configure rocprofiler. Must run BEFORE HIP finishes initializing (§5.2.3) —
// after that the SDK refuses and CAJETA_ROCM_LATE is the honest answer.
int32_t     __cajeta_prof_rocm_configure(void);
int32_t     __cajeta_prof_rocm_configured(void);
int32_t     __cajeta_prof_rocm_tool_init_ran(void);

// ── Unit 8.2.c: buffered kernel-dispatch tracing ──────────────────────────
//
// The device's answer arrives LATER than the launch that caused it — the SDK
// buffers dispatch records and hands them over in batches. So the launch id
// rides along as the SDK's external correlation id, and a record coming back
// is matched to the launch that is waiting for it. The alternative, flushing
// and waiting at each launch, would serialize concurrent streams and destroy
// the overlap the trace exists to show (§5.1.3).
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
// §6.4 — BOOTTIME minus MONOTONIC is constant while the machine is awake and
// jumps by the sleep duration when it suspends. A trace spanning a suspend has
// everything after the sleep sitting minutes out of place, in a file that
// otherwise renders perfectly. The decision is separated from the sampling so
// it can be exercised without suspending the machine running the test.
int32_t     __cajeta_prof_rocm_note_clock_offset(int64_t offsetNs);
int32_t     __cajeta_prof_rocm_suspended(void);
int64_t     __cajeta_prof_rocm_suspend_ns(void);

// Hand a device record back to the launch that is waiting for it. Declared
// here because the ROCm backend is compiled BEFORE the GPU seam in the single
// runtime TU and its buffer callback has to reach forward. Timestamps arrive
// already mapped into the host clock domain (§5.1.7). Returns 1 if a waiting
// launch claimed it, 0 if none did.
int32_t     __cajeta_prof_gpu_resolve_dispatch(int64_t launchId,
                                               int64_t devStartNs, int64_t devEndNs);
// The tier-explicit form: the caller names the MECHANISM that supplied the
// span (§5.1.4) — the ROCm callback resolves vendor dispatch records at
// TIER_DEVICE (the shim above), the Vulkan dispatcher resolves query-pool
// brackets at TIER_EVENT.
int32_t     __cajeta_prof_gpu_resolve_dispatch_tier(int64_t launchId,
                                                    int64_t devStartNs,
                                                    int64_t devEndNs,
                                                    int32_t tier);
// The full form: producer-known §11.3 flags ride the event and are OR'd with
// the checker's own findings at emit.
int32_t     __cajeta_prof_gpu_resolve_dispatch_flags(int64_t launchId,
                                                     int64_t devStartNs,
                                                     int64_t devEndNs,
                                                     int32_t tier,
                                                     int32_t integrityFlags);

// ── Unit 13: the Vulkan backend (spec §5.5, §6.5, §6.6) ───────────────────
//
// Split like the ROCm backend, but along a different line: the PURE half
// (cajeta_rt_prof_vulkan.c — family selection, wrap arithmetic, the low-power
// timestamp-reset detector, period validation) compiles with no Vulkan SDK at
// all, and the API half lives in cajeta_xpu_vulkan.c beside the dispatch path
// that owns the command buffer the bracket is recorded into.
int32_t  __cajeta_xpu_vk_pick_queue_family(const uint32_t* queueFlags,
                                           const uint32_t* timestampValidBits,
                                           int32_t n, int32_t* timingOk);
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
// The seam half (cajeta_rt_prof_gpu.c): the launch id the dispatcher brackets
// under, the bracket hand-back, and §5.5.8's explicit host-blocked span.
int64_t  __cajeta_prof_vk_current_launch(void);
void     __cajeta_prof_vk_bracket_resolved(int64_t launchId, int64_t devStartNs,
                                           int64_t devEndNs, int32_t flags);
int32_t  __cajeta_prof_vk_note_wait(int64_t queue, int64_t startNs,
                                    int64_t endNs);

// ── CUDA event-tier fallback: device timing with no CUDA Toolkit ─────────
//
// CUPTI ships with the Toolkit; cuEventRecord / cuEventElapsedTime ship with
// the DRIVER, which every machine that can run a kernel already has. So the
// tier a driver-only machine can reach is EVENT — device event bracketing,
// exactly what Vulkan falls back to when no vendor layer is present — rather
// than HOST, which is all it used to get.
//
// Same split as Vulkan's above: the DISPATCHER owns the events (only it has
// the driver's entry points) and hands finished brackets back here; the seam
// owns the launch id and the arming state. The dependency runs one way only —
// the xpu translation units are textually included AFTER the profiler's, so
// the profiler can never call into them.
int64_t  __cajeta_prof_cuda_current_launch(void);
void     __cajeta_prof_cuda_bracket_resolved(int64_t launchId, int64_t devStartNs,
                                             int64_t devEndNs);
// Armed by the CUDA loader once the event entry points are bound and the
// reference event that anchors spans to the host clock has been established.
// `why` is kept verbatim for the operator: every state owes a sentence.
void     __cajeta_prof_cuda_events_note(int32_t ok, const char* why);
int32_t  __cajeta_prof_cuda_events_ok(void);
const char* __cajeta_prof_cuda_events_reason(void);
int64_t  __cajeta_prof_cuda_event_spans(void);

// ── Unit 12: CUPTI binding state (spec §5.4) ──────────────────────────────
//
// The same shape as the ROCm backend's, for the same §5.2.2-class reason: a
// run whose CUPTI is missing must still succeed, degraded and reported, and a
// cupti table that binds nothing yet answers calls would read downstream
// exactly like a working device backend. Unit 1's §5.4.4 verdict (what an
// UNPRIVILEGED user may be promised) gates the capability-ladder claims, not
// this loader — which is why the loader lands first.
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
// §6.2/§6.9 — the timestamp callback is newer than the core Activity API
// (CUDA 11.6). Bound separately and reported rather than folded into the
// all-or-nothing core set: its ABSENCE selects the conversion path, it does
// not make the backend absent.
int32_t     __cajeta_prof_cupti_has_timestamp_callback(void);
// §10.5/§12.5 — WSL is where the timestamp callback was measured accepted-
// then-ignored, so the platform has to be identifiable. The parse is split
// from the probe so it is testable on machines that are not WSL.
int32_t     __cajeta_prof_cupti_version_is_wsl(const char* procVersion);
int32_t     __cajeta_prof_cupti_on_wsl(void);
/* 12.2.d — the external-correlation chokepoint, called from the CUDA vtbl's
 * begin/end. `pushes`/`pops` count ATTEMPTS at entry, so a test can tell that
 * the launch path is wired to them apart from their working when called
 * directly. */
// Arming, which binding is NOT: registers the activity buffer callbacks and
// enables the kinds whose records the seam resolves launches with. Until this
// runs, kinds_enabled is 0, so tracing() is false and every CUDA launch
// publishes at host tier. The ROCm counterpart is __cajeta_prof_rocm_configure.
int32_t     __cajeta_prof_cupti_configure(void);
int32_t     __cajeta_prof_cupti_configured(void);
int32_t     __cajeta_prof_cupti_tracing(void);
int32_t     __cajeta_prof_cupti_push(int64_t launchId);
int32_t     __cajeta_prof_cupti_pop(void);
int64_t     __cajeta_prof_cupti_pushes(void);
int64_t     __cajeta_prof_cupti_pops(void);
int32_t     __cajeta_prof_cupti_flush(void);
// How many launches are parked waiting for a device record, and the two ways a
// parked launch can end up published at host tier instead: the table was full
// when it launched (overflow), or its record never came back (unclaimed).
int32_t     __cajeta_prof_gpu_collect(int32_t backend);
// ── Unit 6.6: GPU capture, armed by the same environment as the sampler ────
//
// A profiled run records GPU work into the SAME trace as its samples (§8.3's
// one time axis, §8.8's one file). Bounded: the ring drops the oldest and
// counts it, the same bargain the sampler's ring makes.
int32_t     __cajeta_prof_gpu_capture_arm(int32_t cap);
void        __cajeta_prof_gpu_capture_disarm(void);
int64_t     __cajeta_prof_gpu_captured(void);
int64_t     __cajeta_prof_gpu_capture_dropped(void);
int32_t     __cajeta_prof_gpu_pending_count(void);
void        __cajeta_prof_gpu_pending_reset(void);
int64_t     __cajeta_prof_gpu_pending_overflow(void);
int64_t     __cajeta_prof_gpu_pending_unclaimed(void);

// ── Unit 9: clock correlation and integrity (spec §6, §11) ────────────────
//
// A correlation DOMAIN is one device clock that has to be mapped onto the host
// timeline. Domains are keyed by the CAJ_XPU_* backend id so a record's
// `backend` field indexes its own correlation directly, plus one synthetic
// domain reserved for tests.
//
// The synthetic domain is not a convenience. §6's failure modes cannot be
// reproduced against the CPU backend, whose offset and drift are both exactly
// zero — a module that unconditionally answered "no offset, no drift" would
// satisfy every CPU-backend assertion that could be written. The synthetic
// domain has an offset and a drift the caller chose, so a wrong answer shows up
// as a wrong number.
#define CAJETA_CLOCK_MAX_DOMAINS   8
#define CAJETA_CLOCK_DOMAIN_SYNTH  7

// A calibration sample is a sandwich: host clock, device clock, host clock.
// The gap between the two host reads bounds how far the device read can be from
// the host instant it is being paired with, so it IS the sample's uncertainty —
// no separate quality metric is needed, and none can be trusted more.
#define CAJETA_CLOCK_OK                 0
#define CAJETA_CLOCK_REJECT_DISPERSION  1   // sandwich wider than the cap (§6.7)
#define CAJETA_CLOCK_REJECT_PERIOD      2   // no plausible period set (§11.4)
#define CAJETA_CLOCK_REJECT_BACKWARD    3   // host clock went backwards mid-sample
#define CAJETA_CLOCK_REJECT_DOMAIN      4   // domain id out of range

// Span integrity (spec §11.3). A bitmask, because a span can be several kinds
// of wrong at once and a consumer that sees only the first reason will
// misreport the rest.
#define CAJETA_SPAN_OK            0
#define CAJETA_SPAN_NONMONOTONIC  (1 << 0)  // started before the previous span did
#define CAJETA_SPAN_NEGATIVE      (1 << 1)  // end precedes start
#define CAJETA_SPAN_IMPLAUSIBLE   (1 << 2)  // duration outside any sane bound
#define CAJETA_SPAN_UNCORRELATED  (1 << 3)  // no trustworthy mapping (§11.6)
#define CAJETA_SPAN_OUTSIDE_HOST  (1 << 4)  // device span escapes its own launch

// Longest duration a single dispatch may plausibly claim. An hour-long kernel
// is a broken timestamp, not a slow kernel; the real ceiling is the driver's
// own watchdog, which is far below this.
#define CAJETA_SPAN_MAX_NS  (60LL * 1000000000LL)

// Bounds on a device timestamp period, in host ns per device tick. Zero and
// negative are the shapes drivers actually report when they do not know; the
// outer bounds catch a units mix-up in either direction (§11.4).
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

// One calibration read, supplied by the backend: fill the sandwich and return
// non-zero on success. The backend owns how to read its device clock; it does
// not own how many times that is worth trying.
typedef int32_t (*CajetaClockReadFn)(int64_t* hostBeforeNs, int64_t* devTicks,
                                     int64_t* hostAfterNs, void* user);

// Sample until `wantSamples` are accepted or `maxAttempts` reads have been
// made, whichever comes first; returns the number accepted. The bound lives
// here rather than in each backend's loop because §6.7's requirement is that
// a device in an unfavourable power state cannot hang the profiler — and a
// bound each caller re-implements is one the next backend forgets.
int32_t __cajeta_prof_clock_calibrate(int32_t domain, CajetaClockReadFn read,
                                      void* user, int32_t wantSamples,
                                      int32_t maxAttempts);

// ── Tier ladder and demotion (spec §10.4, §11.1, §11.2) ───────────────────
//
// The ladder is DEVICE -> EVENT -> HOST and it is walked one rung at a time.
// HOST is the floor and there is no rung below it: a host submit-to-complete
// window always exists, so there is always something honest left to report and
// never a reason to fail the run (§10.4). What matters to a consumer is not
// only the tier but WHY it fell, and the FIRST reason is the real one — later
// demotions are consequences of already being degraded.
#define CAJETA_DEMOTE_NONE           0
#define CAJETA_DEMOTE_STARTUP_CHECK  1   // §11.1 self-verification failed
#define CAJETA_DEMOTE_NO_RECORDS     2   // §11.2 accepted launches, delivered none
#define CAJETA_DEMOTE_BAD_PERIOD     3   // §11.4 driver's period was implausible
#define CAJETA_DEMOTE_NO_CLOCK       4   // §11.6 no trustworthy correlation
#define CAJETA_DEMOTE_NODE           5   // §10.2 device node absent or closed

// How many launches a backend may accept while delivering nothing before its
// timing is disabled (§11.2). The default is deliberately small: a backend
// that is going to deliver records delivers them for the first dispatch.
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

// §11.1 — startup self-verification over a handful of real dispatches: end must
// exceed start, durations must be sane, and consecutive dispatches must produce
// DIFFERENT timestamps. That last one is the stuck-counter check, and nothing
// else catches it: every span on its own is perfectly well formed.
int32_t __cajeta_prof_tier_verify(int32_t domain, const int64_t* startsNs,
                                  const int64_t* endsNs, int32_t n);

// ── Undo stack for partial initialization (spec §10.3) ────────────────────
//
// Setup enables facilities in order; a failure partway must leave NOTHING
// enabled rather than a half-configured backend. Steps unwind in reverse,
// because step 3 may depend on what step 1 established.
#define CAJETA_UNDO_MAX_STEPS 16

typedef void (*CajetaUndoFn)(void* user);

int32_t __cajeta_prof_undo_push(int32_t domain, CajetaUndoFn fn, void* user);
int32_t __cajeta_prof_undo_depth(int32_t domain);
int32_t __cajeta_prof_undo_unwind(int32_t domain);
int32_t __cajeta_prof_undo_commit(int32_t domain);

// ── Device node diagnosis (spec §10.2) ────────────────────────────────────
//
// "Not installed" and "installed but you lack permission" have completely
// different fixes, and reporting the second as the first sends a developer off
// to reinstall a driver they already have.
#define CAJETA_NODE_OK            0
#define CAJETA_NODE_ABSENT        1
#define CAJETA_NODE_INACCESSIBLE  2

int32_t __cajeta_prof_probe_node(const char* path);
const char* __cajeta_prof_node_advice(int32_t status, const char* path);

// Cross-check one dispatch record against itself: the device span has to sit
// inside the host submit-to-complete window that produced it. This is the
// check that catches a whole clock domain being wrong — §6.5 measured two
// backends' preferred domains 5.68 s apart, and a lane converted with the
// wrong domain still renders as a perfectly ordinary span. Only comparing it
// against the host window it came from reveals it.
int32_t __cajeta_prof_check_dispatch(const CajetaGpuEvent* ev);

// ── Clock snapshots (spec §7.5) ───────────────────────────────────────────
//
// Each completed calibration records the host/device pair it was anchored on,
// so the mapping is reproducible FROM THE TRACE rather than only from the
// process that wrote it. Without this a reader has the converted timestamps
// and no way to check them, which is the same position §11.6 refuses to put a
// consumer in.
//
// Perfetto clock ids [64,127] are user-defined and SEQUENCE-SCOPED — valid
// only within the packet sequence that emitted the snapshot — so a domain's id
// is 64 + domain and the writer must keep them on one sequence.
#define CAJETA_CLOCK_PERFETTO_BASE_ID 64
#define CAJETA_CLOCK_MAX_SNAPSHOTS    32

typedef struct {
    int32_t domain;
    int32_t generation;   // which calibration round produced it
    int64_t hostNs;
    int64_t devTicks;
} CajetaClockSnapshot;

// §7.8 — driver identity and active layers, supplied by the backend that owns
// the domain. Copied rather than aliased: a backend that hands over a string
// from a vendor library has no obligation to keep it alive until drain, and the
// trace is written long after initialization.
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
