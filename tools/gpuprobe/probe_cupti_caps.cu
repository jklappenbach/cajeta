// probe_cupti_caps.cu — NVIDIA profiling capability audit for the cajeta profiler.
//
// Settles the one load-bearing inference left in the GPU kernel-timing
// research: whether CUDA events and CUPTI Activity kernel timestamps are
// exempt from NVIDIA's profiling permission gate
// (NVreg_RestrictProfilingToAdminUsers / RmProfilingAdminOnly), which decides
// whether our baseline CUDA timing tier works for unprivileged users.
//
// The proof requires all three of:
//   T1  cuEventElapsedTime returns a sane duration
//   T2  CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL records arrive with start != 0
//   T3  the Profiling API path fails with a privilege error IN THE SAME PROCESS
//
// T3 is what makes T1/T2 meaningful. If T3 *succeeds*, this process is
// privileged (or the gate is off) and T1/T2 prove nothing about the exemption
// — the run is INCONCLUSIVE, not a pass. The caller must report the gate state.
//
// Also measured, to replace documentation-derived figures in the research:
//   T4  cuptiActivityRegisterTimestampCallback actually takes effect
//   T5  per-launch overhead: untimed vs event-bracketed vs CUPTI-traced
//
// Exit codes: 0 = ran to completion (read the RESULT lines), 1 = could not
// initialize CUDA at all. Never fails on a negative finding — this is an
// audit, not a gate.
//
// Output contract: every finding is a `RESULT key=value` line so the workflow
// can grep a verdict without parsing prose.
//
// Build (Linux):
//   nvcc -arch=native -O2 probe_cupti_caps.cu -o probe_cupti_caps \
//        -I$CUDA_HOME/extras/CUPTI/include -L$CUDA_HOME/extras/CUPTI/lib64 -lcupti -lcuda
// Build (Windows, inside vcvars64):
//   nvcc -arch=native -O2 probe_cupti_caps.cu -o probe_cupti_caps.exe ^
//        -I"%CUDA_PATH%\extras\CUPTI\include" -L"%CUDA_PATH%\extras\CUPTI\lib64" -lcupti -lcuda

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include <cuda.h>
#include <cuda_runtime.h>
#include <cupti.h>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <time.h>
#endif

// The Profiling API lives in its own header and is the T3 subject. Guard it so
// the probe still builds if a toolkit ships without it.
#if defined(__has_include)
  #if __has_include(<cupti_profiler_target.h>)
    #include <cupti_profiler_target.h>
    #define HAVE_PROFILER_TARGET 1
  #endif
#endif

// ---------------------------------------------------------------- utilities

static int g_fail = 0;

#define CU_CHECK(call, what)                                                   \
    do {                                                                       \
        CUresult _r = (call);                                                  \
        if (_r != CUDA_SUCCESS) {                                              \
            const char* _s = nullptr; cuGetErrorName(_r, &_s);                 \
            printf("ERROR %s -> %s (%d)\n", what, _s ? _s : "?", (int) _r);     \
            g_fail = 1;                                                        \
        }                                                                      \
    } while (0)

// CUPTI calls are reported, never fatal — a refused call IS the finding.
static CUptiResult cupti_try(CUptiResult r, const char* what) {
    if (r != CUPTI_SUCCESS) {
        const char* s = nullptr;
        cuptiGetResultString(r, &s);
        printf("  cupti: %-46s -> %s (%d)\n", what, s ? s : "?", (int) r);
    }
    return r;
}

static uint64_t host_ns(void) {
#if defined(_WIN32)
    // Windows has no clock_gettime; QPC is both our timebase and CUPTI's
    // default here — which is itself a finding for the profiler design.
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (uint64_t) ((long double) t.QuadPart * 1e9L / (long double) freq.QuadPart);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t) t.tv_sec * 1000000000ull + (uint64_t) t.tv_nsec;
#endif
}

// T4's subject: hand CUPTI our own clock and see whether records adopt it.
static uint64_t CUPTIAPI cajeta_timestamp(void) { return host_ns(); }

// ------------------------------------------------------------ activity sink

// CUPTI requires 8-byte-aligned buffers. malloc already guarantees at least
// max_align_t (16 bytes on both MSVC and glibc), so no manual adjustment is
// needed — and adjusting would mean free() receiving a pointer malloc never
// returned. 4 MiB matches Kineto; NVIDIA suggests 1-10 MiB.
#define BUF_SIZE (4 * 1024 * 1024)

static int      g_kernel_records   = 0;
static int      g_zero_timestamps  = 0;
static int      g_bad_order        = 0;
static int      g_driver_records   = 0;
static uint64_t g_first_start      = 0;
static uint64_t g_first_end        = 0;
static uint32_t g_first_corr       = 0;
static char     g_first_name[256]  = {0};
static size_t   g_dropped          = 0;

static void CUPTIAPI buffer_requested(uint8_t** buffer, size_t* size, size_t* maxNumRecords) {
    uint8_t* raw = (uint8_t*) malloc(BUF_SIZE);
    if (!raw) { *buffer = nullptr; *size = 0; *maxNumRecords = 0; return; }
    *buffer        = raw;
    *size          = BUF_SIZE;
    *maxNumRecords = 0;   // pack maximally
}

static void CUPTIAPI buffer_completed(CUcontext ctx, uint32_t streamId,
                                      uint8_t* buffer, size_t size, size_t validSize) {
    (void) size;
    CUpti_Activity* rec = nullptr;
    if (validSize > 0) {
        for (;;) {
            CUptiResult r = cuptiActivityGetNextRecord(buffer, validSize, &rec);
            if (r == CUPTI_ERROR_MAX_LIMIT_REACHED) break;
            if (r != CUPTI_SUCCESS) break;

            if (rec->kind == CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL) {
                // Read PREFIX FIELDS ONLY. CUPTI appends fields across record
                // versions but never reorders, so the leading members are
                // stable across Kernel8/9/10. If a toolkit predates Kernel9,
                // change this cast — nvcc --version is printed by the workflow.
                CUpti_ActivityKernel9* k = (CUpti_ActivityKernel9*) rec;
                g_kernel_records++;
                if (k->start == 0 || k->end == 0)  g_zero_timestamps++;
                if (k->end <= k->start)            g_bad_order++;
                if (g_first_start == 0) {
                    g_first_start = k->start;
                    g_first_end   = k->end;
                    g_first_corr  = k->correlationId;
                    if (k->name) { strncpy(g_first_name, k->name, sizeof(g_first_name) - 1); }
                }
            } else if (rec->kind == CUPTI_ACTIVITY_KIND_DRIVER) {
                g_driver_records++;
            }
        }
    }
    size_t dropped = 0;
    if (cuptiActivityGetNumDroppedRecords(ctx, streamId, &dropped) == CUPTI_SUCCESS && dropped)
        g_dropped += dropped;
    free(buffer);
}

// ------------------------------------------------------------------- kernel

__global__ void spin_kernel(float* out, int iters) {
    float acc = 0.0f;
    for (int i = 0; i < iters; ++i) acc = fmaf(acc, 1.000001f, 1.0f);
    if (threadIdx.x == 0 && blockIdx.x == 0) *out = acc;
}

// --------------------------------------------------------------------- main

int main(int argc, char** argv) {
    const int iters   = (argc > 1) ? atoi(argv[1]) : 200000;
    const int reps    = (argc > 2) ? atoi(argv[2]) : 200;

    printf("=== cajeta NVIDIA profiling capability audit ===\n\n");

    // ---- environment ------------------------------------------------------
    int driver_version = 0, runtime_version = 0;
    cudaDriverGetVersion(&driver_version);
    cudaRuntimeGetVersion(&runtime_version);
    uint32_t cupti_version = 0;
    cuptiGetVersion(&cupti_version);
    printf("RESULT cuda_driver_version=%d\n", driver_version);
    printf("RESULT cuda_runtime_version=%d\n", runtime_version);
    printf("RESULT cupti_api_version=%u\n", cupti_version);

    CU_CHECK(cuInit(0), "cuInit");
    if (g_fail) { printf("RESULT verdict=CUDA_INIT_FAILED\n"); return 1; }

    CUdevice dev;
    CU_CHECK(cuDeviceGet(&dev, 0), "cuDeviceGet");
    char name[256] = {0};
    cuDeviceGetName(name, sizeof(name), dev);
    int cc_major = 0, cc_minor = 0;
    cuDeviceGetAttribute(&cc_major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev);
    cuDeviceGetAttribute(&cc_minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev);
    printf("RESULT device_name=%s\n", name);
    printf("RESULT compute_capability=%d.%d\n", cc_major, cc_minor);

    CUcontext ctx;
    CU_CHECK(cuCtxCreate(&ctx, 0, dev), "cuCtxCreate");
    if (g_fail) { printf("RESULT verdict=CONTEXT_FAILED\n"); return 1; }

    float* d_out = nullptr;
    cudaMalloc((void**) &d_out, sizeof(float));

    // ---- T1: CUDA events --------------------------------------------------
    printf("\n--- T1: CUDA events (driver API) ---\n");
    CUevent ev0, ev1;
    CUresult e0 = cuEventCreate(&ev0, CU_EVENT_DEFAULT);
    CUresult e1 = cuEventCreate(&ev1, CU_EVENT_DEFAULT);
    int t1_ok = 0;
    float ev_ms = 0.0f;
    if (e0 == CUDA_SUCCESS && e1 == CUDA_SUCCESS) {
        cuEventRecord(ev0, 0);
        spin_kernel<<<64, 256>>>(d_out, iters);
        cuEventRecord(ev1, 0);
        CUresult sync = cuEventSynchronize(ev1);
        CUresult el   = cuEventElapsedTime(&ev_ms, ev0, ev1);
        if (sync == CUDA_SUCCESS && el == CUDA_SUCCESS && ev_ms > 0.0f) t1_ok = 1;
        else {
            const char* s = nullptr; cuGetErrorName(el, &s);
            printf("  elapsed-time call -> %s\n", s ? s : "?");
        }
    }
    printf("RESULT t1_events_work=%s\n", t1_ok ? "YES" : "NO");
    printf("RESULT t1_event_ms=%.6f\n", ev_ms);

    // ---- T4 + T2: timestamp callback, then activity records ---------------
    // Registration MUST precede every cuptiActivityEnable call, so T4 is set
    // up here even though its verdict is read after T2's records arrive.
    printf("\n--- T4: cuptiActivityRegisterTimestampCallback ---\n");
    uint64_t host_before = host_ns();
    CUptiResult tscb = cupti_try(cuptiActivityRegisterTimestampCallback(cajeta_timestamp),
                                 "cuptiActivityRegisterTimestampCallback");
    printf("RESULT t4_timestamp_callback_accepted=%s\n", tscb == CUPTI_SUCCESS ? "YES" : "NO");

    printf("\n--- T2: CUPTI Activity kernel records ---\n");
    cupti_try(cuptiActivityRegisterCallbacks(buffer_requested, buffer_completed),
              "cuptiActivityRegisterCallbacks");
    // CONCURRENT_KERNEL, never KERNEL — the latter serializes all execution.
    CUptiResult en_k = cupti_try(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL),
                                 "cuptiActivityEnable(CONCURRENT_KERNEL)");
    // DRIVER is required for correlation ids to exist at all.
    cupti_try(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_DRIVER),
              "cuptiActivityEnable(DRIVER)");

    for (int i = 0; i < 5; ++i) spin_kernel<<<64, 256>>>(d_out, iters);
    cudaDeviceSynchronize();
    cupti_try(cuptiActivityFlushAll(CUPTI_ACTIVITY_FLAG_FLUSH_FORCED), "cuptiActivityFlushAll");
    uint64_t host_after = host_ns();

    printf("RESULT t2_activity_enable_ok=%s\n", en_k == CUPTI_SUCCESS ? "YES" : "NO");
    printf("RESULT t2_kernel_records=%d\n", g_kernel_records);
    printf("RESULT t2_driver_records=%d\n", g_driver_records);
    printf("RESULT t2_zero_timestamps=%d\n", g_zero_timestamps);
    printf("RESULT t2_bad_ordering=%d\n", g_bad_order);
    printf("RESULT t2_dropped_records=%zu\n", g_dropped);
    printf("RESULT t2_first_kernel_name=%s\n", g_first_name[0] ? g_first_name : "(none)");
    printf("RESULT t2_first_correlation_id=%u\n", g_first_corr);
    int t2_ok = (g_kernel_records > 0 && g_zero_timestamps == 0 && g_bad_order == 0);
    printf("RESULT t2_activity_timestamps_work=%s\n", t2_ok ? "YES" : "NO");

    // T4 verdict: if our callback took effect, record timestamps sit inside the
    // host window we bracketed. If CUPTI ignored it, they are in its own domain
    // (CLOCK_REALTIME on Linux, QPC on Windows) and will fall far outside.
    if (g_first_start) {
        int inside = (g_first_start >= host_before && g_first_end <= host_after);
        printf("RESULT t4_records_in_our_clock_domain=%s\n", inside ? "YES" : "NO");
        printf("RESULT t4_host_window_ns=%llu\n",
               (unsigned long long) (host_after - host_before));
        printf("RESULT t4_first_kernel_start=%llu\n", (unsigned long long) g_first_start);
        printf("RESULT t4_first_kernel_dur_ns=%llu\n",
               (unsigned long long) (g_first_end - g_first_start));
    } else {
        printf("RESULT t4_records_in_our_clock_domain=NO_RECORDS\n");
    }

    // ---- T5: overhead ------------------------------------------------------
    // Each mechanism is measured against the SAME untraced baseline, with the
    // other mechanism OFF. Small kernels keep launch cost visible.
    //
    // Corrected 2026-08-21 after the first shakedown run (32439821390). The
    // original measured event bracketing while CUPTI activity tracing was still
    // enabled, and then reported `traced_events - traced_plain` as
    // "event_overhead" — which is the MARGINAL cost of adding events on top of
    // CUPTI, not the cost of event bracketing. It understated event bracketing
    // against an untraced program by 4-5x (WSL reported 3258 ns where the true
    // delta from untraced was 16490 ns), and it made the two mechanisms
    // non-comparable, which is the one thing this test exists to do: the
    // research claim is that CUPTI is the CHEAPER mechanism, and that claim
    // cannot rest on a measurement of one taken while the other is running.
    //
    // A warm-up pass is discarded per bench. Without it the untraced run — which
    // must run last, after the CUPTI disables — inherited warm caches from its
    // predecessors and read low, inflating both overhead figures.
    printf("\n--- T5: per-launch overhead ---\n");
    auto bench_once = [&](int use_events) -> double {
        // Warm-up, discarded: same shape as the measured loop.
        for (int i = 0; i < 32; ++i) {
            if (use_events) {
                CUevent w0, w1;
                cuEventCreate(&w0, CU_EVENT_DEFAULT);
                cuEventCreate(&w1, CU_EVENT_DEFAULT);
                cuEventRecord(w0, 0);
                spin_kernel<<<1, 32>>>(d_out, 1);
                cuEventRecord(w1, 0);
                cuEventDestroy(w0);
                cuEventDestroy(w1);
            } else {
                spin_kernel<<<1, 32>>>(d_out, 1);
            }
        }
        cudaDeviceSynchronize();
        uint64_t t0 = host_ns();
        for (int i = 0; i < reps; ++i) {
            if (use_events) {
                CUevent a, b;
                cuEventCreate(&a, CU_EVENT_DEFAULT);
                cuEventCreate(&b, CU_EVENT_DEFAULT);
                cuEventRecord(a, 0);
                spin_kernel<<<1, 32>>>(d_out, 1);
                cuEventRecord(b, 0);
                cuEventDestroy(a);
                cuEventDestroy(b);
            } else {
                spin_kernel<<<1, 32>>>(d_out, 1);
            }
        }
        cudaDeviceSynchronize();
        return (double) (host_ns() - t0) / (double) reps;
    };

    // Repetition with a median (2026-08-21, plan item 1.2.d). A single timed
    // loop was not precise enough under WSL2's GPU paravirtualization: run
    // 32485085982 reported a NEGATIVE CUPTI overhead (-1212 ns, the traced run
    // "faster" than untraced) and a combined events+CUPTI figure BELOW event
    // bracketing alone. Both are impossible, and both say the same thing —
    // run-to-run variance exceeded the ~4 us effect being measured.
    //
    // The median resists the occasional long run that a mean would absorb
    // silently. The spread is reported alongside every figure so a measurement
    // that cannot be trusted announces itself instead of being read as a
    // finding: a delta smaller than the spread of its own inputs is noise, and
    // the reader can now see that without re-running anything.
    const int T5_REPS = 7;
    auto bench = [&](int use_events, double* spread_out) -> double {
        double v[T5_REPS];
        for (int i = 0; i < T5_REPS; ++i) v[i] = bench_once(use_events);
        for (int i = 1; i < T5_REPS; ++i) {           // insertion sort, N=7
            double key = v[i];
            int j = i - 1;
            while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; --j; }
            v[j + 1] = key;
        }
        if (spread_out) *spread_out = v[T5_REPS - 1] - v[0];
        return v[T5_REPS / 2];
    };

    // CUPTI on: plain launches, and launches with events on top (the combined
    // cost, reported separately and never confused with either mechanism alone).
    double sp_tp = 0, sp_te = 0, sp_up = 0, sp_ue = 0;
    double traced_plain     = bench(0, &sp_tp);
    double traced_events    = bench(1, &sp_te);
    cuptiActivityFlushAll(CUPTI_ACTIVITY_FLAG_FLUSH_FORCED);

    // CUPTI off: the shared baseline, and event bracketing measured ALONE.
    cuptiActivityDisable(CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL);
    cuptiActivityDisable(CUPTI_ACTIVITY_KIND_DRIVER);
    double untraced_plain  = bench(0, &sp_up);
    double untraced_events = bench(1, &sp_ue);

    printf("RESULT t5_ns_per_launch_untraced=%.1f\n", untraced_plain);
    printf("RESULT t5_ns_per_launch_cupti_traced=%.1f\n", traced_plain);
    printf("RESULT t5_ns_per_launch_event_bracketed=%.1f\n", untraced_events);
    printf("RESULT t5_ns_per_launch_events_plus_cupti=%.1f\n", traced_events);
    // Both overheads are against the SAME untraced baseline, so they compare.
    printf("RESULT t5_cupti_overhead_ns=%.1f\n", traced_plain - untraced_plain);
    printf("RESULT t5_event_overhead_ns=%.1f\n", untraced_events - untraced_plain);
    // Kept explicit rather than left to be derived: the marginal cost of adding
    // events to an already-CUPTI-traced run, which is what the pre-2026-08-21
    // probe mislabelled as t5_event_overhead_ns.
    printf("RESULT t5_event_marginal_over_cupti_ns=%.1f\n", traced_events - traced_plain);
    // The event bracket here records and destroys; it does not call
    // cuEventElapsedTime. That is deliberate — this measures the LAUNCH-PATH
    // cost a profiler pays per dispatch, not the host-side cost of reading the
    // result back, which a real implementation batches. Stated because the
    // number would otherwise look low against published event-timing figures.
    printf("RESULT t5_event_bracket_reads_elapsed=NO\n");
    // Every figure above is the MEDIAN of T5_REPS runs; these are the observed
    // spreads (max-min) of the same samples. An overhead smaller than the spread
    // of either input is not a measurement, and the run says so rather than
    // leaving the reader to assume precision it does not have.
    printf("RESULT t5_reps=%d\n", T5_REPS);
    printf("RESULT t5_spread_untraced_ns=%.1f\n", sp_up);
    printf("RESULT t5_spread_cupti_traced_ns=%.1f\n", sp_tp);
    printf("RESULT t5_spread_event_bracketed_ns=%.1f\n", sp_ue);
    printf("RESULT t5_spread_events_plus_cupti_ns=%.1f\n", sp_te);
    {
        double worst = sp_up > sp_tp ? sp_up : sp_tp;
        double cupti_ovh = traced_plain - untraced_plain;
        double mag = cupti_ovh < 0 ? -cupti_ovh : cupti_ovh;
        printf("RESULT t5_cupti_overhead_exceeds_noise=%s\n",
               mag > worst ? "YES" : "NO");
    }

    // ---- T3: the Profiling API privilege gate ------------------------------
    // THE DECIDING TEST. T1/T2 only prove the exemption if this is REFUSED.
    printf("\n--- T3: Profiling API privilege gate ---\n");
    const char* t3 = "NOT_BUILT";
#ifdef HAVE_PROFILER_TARGET
    {
        CUpti_Profiler_Initialize_Params ip = { CUpti_Profiler_Initialize_Params_STRUCT_SIZE };
        CUptiResult pinit = cupti_try(cuptiProfilerInitialize(&ip), "cuptiProfilerInitialize");

        CUptiResult probe = pinit;
        if (pinit == CUPTI_SUCCESS) {
            // GetCounterAvailability touches the counter hardware and is where
            // the gate bites, without needing a full session's config images.
            CUpti_Profiler_GetCounterAvailability_Params ga =
                { CUpti_Profiler_GetCounterAvailability_Params_STRUCT_SIZE };
            ga.ctx = ctx;
            probe = cupti_try(cuptiProfilerGetCounterAvailability(&ga),
                              "cuptiProfilerGetCounterAvailability");
        }

        if (probe == CUPTI_ERROR_INSUFFICIENT_PRIVILEGES)      t3 = "REFUSED_PRIVILEGES";
        else if (probe == CUPTI_SUCCESS)                       t3 = "ALLOWED";
        else                                                   t3 = "REFUSED_OTHER";
        printf("RESULT t3_profiler_raw_status=%d\n", (int) probe);
    }
#else
    printf("  cupti_profiler_target.h not present at build time\n");
#endif
    printf("RESULT t3_profiling_api=%s\n", t3);

    // ---- verdict -----------------------------------------------------------
    // The exemption is PROVEN only when the gate is demonstrably active and
    // timing still works. Anything else is inconclusive, and says so.
    const char* verdict;
    if (t1_ok && t2_ok && strcmp(t3, "REFUSED_PRIVILEGES") == 0)
        verdict = "EXEMPTION_PROVEN";           // timing works, counters refused
    else if (t1_ok && t2_ok && strcmp(t3, "ALLOWED") == 0)
        verdict = "INCONCLUSIVE_PROCESS_PRIVILEGED";
    else if (t1_ok && t2_ok)
        verdict = "TIMING_OK_GATE_STATE_UNKNOWN";
    else
        verdict = "TIMING_BROKEN";

    printf("\nRESULT verdict=%s\n", verdict);
    printf("\n");
    if (strcmp(verdict, "INCONCLUSIVE_PROCESS_PRIVILEGED") == 0) {
        printf("NOTE: the Profiling API was ALLOWED, so this process is privileged\n"
               "      or the gate is disabled. T1/T2 passing proves nothing about\n"
               "      unprivileged users. Re-run as a non-admin account with the\n"
               "      driver at its default setting.\n");
    }

    cudaFree(d_out);
    cuCtxDestroy(ctx);
    return 0;
}
