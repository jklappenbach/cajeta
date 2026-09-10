// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
// The Vulkan profiler backend's PURE half: arithmetic and policy with no Vulkan
// types, so it builds with no SDK header. The API half is cajeta_xpu_vulkan.c.

#ifndef CAJETA_PROF_TRACE_STANDALONE

// VkQueueFlagBits values, spelled as raw bits for the same no-SDK reason.
#define CAJ_VK_QUEUE_COMPUTE_BIT 0x2u

typedef struct {
    int32_t  configured;      // configure() accepted a family + period
    int32_t  timing_ok;       // != 0: brackets may claim device ticks
    uint32_t valid_bits;
    double   period_ns;       // advertised; the clock engine's fit refines it
    // Reset tracking over ONE ordered stream: the submit mutex serializes the
    // dispatch path, so device brackets complete in order.
    uint64_t last_end_ticks;
    int32_t  have_last;
    int64_t  resets_detected;
    // An unavailable query pair degrades its span to the host window, not a drop.
    int64_t  spans;
    int64_t  unavailable;
} CajProfVkState;

static CajProfVkState caj_pvk;

// The queue family to dispatch on, or -1 when there is no compute family. A
// family with timestampValidBits == 0 returns values that mean NOTHING, so
// `*timingOk` clears and the run degrades to host windows instead of refusing.
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

// ── Which Vulkan-on-Metal ICD to run on ────────────────────────────────────
// VkDriverId values, raw: VK_DRIVER_ID_MESA_KOSMICKRISP is new in the headers.
#define CAJ_VK_DRIVER_MOLTENVK    14u
#define CAJ_VK_DRIVER_KOSMICKRISP 28u

static int32_t caj_vk_driver_rank(uint32_t id) {
    return id == CAJ_VK_DRIVER_KOSMICKRISP ? 2
         : id == CAJ_VK_DRIVER_MOLTENVK    ? 1 : 0;
}

// The device index to run on, or -1 when there is none. The loader's order must
// not decide on a Mac carrying both ICDs, so rank picks; elsewhere index 0 wins.
// `force` beats the rank and refuses rather than falling back to the other ICD.
int32_t __cajeta_xpu_vk_pick_device(const uint32_t* driverIds, int32_t n,
                                    const char* force) {
    if (!driverIds || n <= 0) return -1;
    uint32_t want = 0;
    if (force && *force) {
        if (strcmp(force, "kosmickrisp") == 0) want = CAJ_VK_DRIVER_KOSMICKRISP;
        else if (strcmp(force, "moltenvk") == 0) want = CAJ_VK_DRIVER_MOLTENVK;
    }
    if (want) {
        for (int32_t i = 0; i < n; ++i)
            if (driverIds[i] == want) return i;
        return -1;
    }
    int32_t best = 0;
    for (int32_t i = 1; i < n; ++i)
        if (caj_vk_driver_rank(driverIds[i]) > caj_vk_driver_rank(driverIds[best]))
            best = i;
    return best;
}

// Separates "no ICD at all" from "an ICD that enumerated nothing", as the two
// want different fixes. Returns the VkResult, spelled raw.
int32_t __cajeta_xpu_vk_classify_init(int32_t loaderFound, int32_t deviceCount) {
    if (!loaderFound) return -9;        // VK_ERROR_INCOMPATIBLE_DRIVER
    if (deviceCount <= 0) return -3;    // VK_ERROR_INITIALIZATION_FAILED
    return 0;                           // VK_SUCCESS
}

// Wrap-correct tick delta at the family's valid-bit width; bits above it may be
// driver garbage. At 64 the mask is all-ones, never the UB of `1ULL << 64`.
uint64_t __cajeta_prof_vk_delta_ticks(uint64_t startTicks, uint64_t endTicks,
                                      uint32_t validBits) {
    if (validBits == 0) return 0;   // meaningless ticks make no duration
    const uint64_t mask = (validBits >= 64)
        ? ~0ULL
        : ((1ULL << validBits) - 1ULL);
    return (endTicks - startTicks) & mask;
}

// Flags a span starting before the previous one ended: on this serialized path
// that is a timestamp-register reset, not concurrency, so tracking then re-bases.
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

// Accepts the selected family's timing parameters, or refuses timing. The
// advertised period only seeds the clock domain; the rolling fit refines it.
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
