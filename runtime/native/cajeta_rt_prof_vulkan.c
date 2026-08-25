// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
//
// cajeta-profiler Unit 13 — the Vulkan backend's PURE half (spec §5.5, §6.5).
//
// Everything here is arithmetic and policy, deliberately free of Vulkan types:
// this file compiles on machines with no Vulkan SDK header, which is where the
// refusal paths matter most (the same reason cajeta_rt_prof_rocm.c declares
// its own slice of the rocprofiler ABI). The Vulkan API mechanics — query
// pools, availability reads, host reset, calibrated timestamps — live in
// cajeta_xpu_vulkan.c, which is included AFTER this file and calls down into
// it; nothing here calls up.
//
// The three behaviours factored out here are exactly the ones a device cannot
// be made to demonstrate on demand:
//
//   §5.5.2 — a queue family with timestampValidBits == 0 accepts timestamp
//   writes and returns values that mean NOTHING. Three of five families on the
//   reference device (RADV STRIX_HALO, measured 2026-08-24) are like this, and
//   the old first-compute-hit selection would time on one wherever a zero-bit
//   family enumerates first.
//
//   §5.5.6 — timestamps wrap at the device's valid-bit width, which is 36 on
//   real hardware and 64 on the reference device and lavapipe; the 64-bit case
//   must not compute `1 << 64`, which is undefined behavior that usually
//   "works".
//
//   §5.5.7 — AMD APUs reset the timestamp register on low-power entry. The
//   span after a reset is internally flawless; only history — a start before
//   the previous end — reveals it.

#ifndef CAJETA_PROF_TRACE_STANDALONE

// VkQueueFlagBits values, spelled as raw bits for the same no-SDK reason.
#define CAJ_VK_QUEUE_COMPUTE_BIT 0x2u

typedef struct {
    int32_t  configured;      // configure() accepted a family + period
    int32_t  timing_ok;       // != 0: brackets may claim device ticks
    uint32_t valid_bits;
    double   period_ns;       // advertised; the clock engine's fit refines it
    // §5.5.7 tracking. One stream of spans: the Vulkan dispatch path is
    // serialized by its submit mutex, so device brackets complete in order.
    uint64_t last_end_ticks;
    int32_t  have_last;
    int64_t  resets_detected;
    // Counters for the run record (§7.8): how many brackets resolved, and how
    // many query pairs came back unavailable (each of those spans degraded to
    // its host window rather than being dropped).
    int64_t  spans;
    int64_t  unavailable;
} CajProfVkState;

static CajProfVkState caj_pvk;

// Pick the queue family whose timestamps mean something (plan 13.2.b).
// Returns the family index to dispatch on, or -1 when no compute family
// exists. `*timingOk` says whether that family can TIME: a compute family
// with zero valid bits still dispatches correctly — refusing the device over
// a timing gap would fail runs §10.4 says must degrade — but timing is
// refused, so the spans that reach the trace stay honest host windows.
int32_t __cajeta_xpu_vk_pick_queue_family(const uint32_t* queueFlags,
                                          const uint32_t* timestampValidBits,
                                          int32_t n, int32_t* timingOk) {
    int32_t firstCompute = -1;
    if (timingOk) *timingOk = 0;
    if (!queueFlags || !timestampValidBits || n <= 0) return -1;
    for (int32_t i = 0; i < n; ++i) {
        if (!(queueFlags[i] & CAJ_VK_QUEUE_COMPUTE_BIT)) continue;
        if (firstCompute < 0) firstCompute = i;
        if (timestampValidBits[i] != 0) {
            if (timingOk) *timingOk = 1;
            return i;
        }
    }
    return firstCompute;
}

// Wrap-correct tick delta at the family's valid-bit width (§5.5.6). Bits
// above the valid width are masked rather than trusted — drivers may leave
// stale garbage there. At 64 bits the mask is all-ones WITHOUT computing
// `1ULL << 64`, which is undefined behavior.
uint64_t __cajeta_prof_vk_delta_ticks(uint64_t startTicks, uint64_t endTicks,
                                      uint32_t validBits) {
    if (validBits == 0) return 0;   // meaningless ticks make no duration
    const uint64_t mask = (validBits >= 64)
        ? ~0ULL
        : ((1ULL << validBits) - 1ULL);
    return (endTicks - startTicks) & mask;
}

// §5.5.7 — flag a span that starts before the previous one ended: on this
// serialized dispatch path that is a timestamp register reset (low-power
// entry), not concurrency. After flagging, tracking re-bases — the counter
// genuinely restarted, and flagging everything after the event forever would
// bury it in noise.
int32_t __cajeta_prof_vk_note_span_ticks(uint64_t startTicks,
                                         uint64_t endTicks) {
    int32_t flags = CAJETA_SPAN_OK;
    if (caj_pvk.have_last && startTicks < caj_pvk.last_end_ticks) {
        flags |= CAJETA_SPAN_NONMONOTONIC;
        caj_pvk.resets_detected++;
    }
    caj_pvk.last_end_ticks = endTicks;
    caj_pvk.have_last = 1;
    return flags;
}

void __cajeta_prof_vk_span_tracking_reset(void) {
    caj_pvk.last_end_ticks = 0;
    caj_pvk.have_last = 0;
}

// Accept the selected family's timing parameters, or refuse timing (§11.4).
// The advertised period seeds the clock engine's domain — the rolling rate
// fit refines it (§6.6), but conversions before the first calibration have to
// start from something, and zero/negative is what a driver reports when it
// does not know.
int32_t __cajeta_prof_vk_configure(uint32_t validBits, double periodNs,
                                   int32_t hasCalibration) {
    (void) hasCalibration;
    caj_pvk.configured = 1;
    caj_pvk.valid_bits = validBits;
    caj_pvk.period_ns = periodNs;
    __cajeta_prof_vk_span_tracking_reset();
    if (validBits == 0
            || !__cajeta_prof_clock_set_period(CAJ_GPU_BACKEND_VULKAN,
                                               periodNs)) {
        caj_pvk.timing_ok = 0;
        return 0;
    }
    caj_pvk.timing_ok = 1;
    return 1;
}

int32_t  __cajeta_prof_vk_timing_ok(void)   { return caj_pvk.timing_ok; }
uint32_t __cajeta_prof_vk_valid_bits(void)  { return caj_pvk.valid_bits; }
double   __cajeta_prof_vk_period_ns(void)   { return caj_pvk.period_ns; }
int64_t  __cajeta_prof_vk_resets(void)      { return caj_pvk.resets_detected; }
int64_t  __cajeta_prof_vk_spans(void)       { return caj_pvk.spans; }
int64_t  __cajeta_prof_vk_unavailable(void) { return caj_pvk.unavailable; }
void     __cajeta_prof_vk_note_resolved(void)    { caj_pvk.spans++; }
void     __cajeta_prof_vk_note_unavailable(void) { caj_pvk.unavailable++; }

void __cajeta_prof_vk_reset(void) {
    memset(&caj_pvk, 0, sizeof(caj_pvk));
}

#endif  /* CAJETA_PROF_TRACE_STANDALONE */
