// probe_cupti_caps.cu — NVIDIA profiling capability audit for the cajeta profiler.
//
// Settles the one load-bearing inference left in the GPU kernel-timing
// research: whether CUDA events and CUPTI Activity kernel timestamps are
// exempt from NVIDIA's profiling permission gate
// (NVreg_RestrictProfilingToAdminUsers / RmProfilingAdminOnly), which decides
// whether our baseline CUDA timing tier works for unprivileged users.
//
// The proof requires all four of:
//   T0  this process does NOT hold the privilege the gate asks for
//   T1  cuEventElapsedTime returns a sane duration
//   T2  CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL records arrive with start != 0
//   T3  something the gate covers is refused for privileges IN THE SAME PROCESS
//
// T0 and T3 are what make T1/T2 meaningful, and T0 is why this probe was
// revised on 2026-08-29. The first version had no T0: it inferred "this process
// is privileged" from T3 being ALLOWED, which is circular — T3 is the thing
// that inference was supposed to help decide. Both audit runs (32485416032,
// 32439821390) then reported INCONCLUSIVE_PROCESS_PRIVILEGED on processes the
// workflow had ALREADY measured as unprivileged in an earlier step, and the two
// measurements were never joined. So T0 is taken here, in-process, and printed
// beside the verdict it feeds.
//
// T3 is likewise no longer a single call. One ALLOWED cannot tell a gate that
// is off from a gate that does not happen to cover the call we picked, so three
// independent gated operations are tried and reported separately. The strongest
// is T3b: gated ACTIVITY KINDS through cuptiActivityEnable — the same call T2
// used for kernel timing, so the only variable is the kind, and a refusal there
// beside T2's success IS the exemption rather than an argument for it.
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
  #include <unistd.h>
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

// Whether THIS process holds the privilege NVIDIA's gate asks for. The verdict
// used to infer this from T3 succeeding, which is circular: it is the thing T3
// is supposed to help decide. Both CI runners were in fact UNPRIVILEGED
// (NT AUTHORITY\NETWORK SERVICE, RUNNER_ELEVATED=False; uid 1000 with no
// passwordless sudo) and still saw the Profiling API ALLOWED, so the inference
// was wrong on the only two boxes it was ever applied to.
static int process_is_privileged() {
#if defined(_WIN32)
    // The gate tests membership in the local Administrators group, so a
    // deny-only SID on a filtered token correctly reads as NOT privileged.
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID admins = nullptr;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                  &admins)) {
        return -1;
    }
    BOOL member = FALSE;
    const BOOL ok = CheckTokenMembership(nullptr, admins, &member);
    FreeSid(admins);
    if (!ok) return -1;
    return member ? 1 : 0;
#else
    if (geteuid() == 0) return 1;
    // Root is not the only key: the driver also accepts CAP_SYS_ADMIN (bit 21),
    // so a capability-granted process must not read as unprivileged.
    FILE* f = fopen("/proc/self/status", "r");
    if (f == nullptr) return -1;
    char line[256];
    int has_sys_admin = -1;
    while (fgets(line, sizeof line, f) != nullptr) {
        unsigned long long eff = 0;
        if (sscanf(line, "CapEff: %llx", &eff) == 1) {
            has_sys_admin = (eff >> 21) & 1ULL ? 1 : 0;
            break;
        }
    }
    fclose(f);
    return has_sys_admin;
#endif
}

static const char* yes_no_unknown(int v) {
    return v < 0 ? "UNKNOWN" : (v ? "YES" : "NO");
}

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
    // An ENUM, not `const int`. MSVC captures a const local into the lambda and
    // then rejects `double v[T5_REPS]` as a non-constant array bound (C2131,
    // "read of a variable outside its lifetime", pointing at `this`). nvcc on
    // Linux accepts the const-int form, so this broke only the Windows job and
    // only in CI. Enumerators are not variables and are not captured.
    enum { T5_REPS = 7 };
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
    printf("RESULT t5_reps=%d\n", (int) T5_REPS);
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

    // ---- T3: is the profiling gate ACTIVE in this process? -----------------
    // THE DECIDING TEST. T1/T2 only prove the exemption if something the gate
    // covers is REFUSED here, in the same process, for privileges.
    //
    // This used to be a single call — cuptiProfilerGetCounterAvailability. It
    // came back ALLOWED on both runners, and the verdict read that as "this
    // process must be privileged". The privilege measurement above says it was
    // not, on either box. So one ALLOWED tells us nothing: a gate that is off,
    // and a gate that simply does not cover the call we picked, look identical
    // through one probe. Three independent gated operations are tried instead,
    // each reported separately, so a null result can be told apart from a blind
    // instrument. The gate is proven ACTIVE if ANY of them refuses.
    const int privileged = process_is_privileged();
    printf("RESULT process_privileged=%s\n", yes_no_unknown(privileged));

    printf("\n--- T3: profiling gate, three independent probes ---\n");

    int t3_refusals = 0;   // operations refused specifically for privileges
    int t3_attempts = 0;   // operations that got far enough to answer

    // T3a — the Profiling API's counter-availability query. Kept as-is so this
    // run stays comparable with the two already in the record.
    const char* t3a = "NOT_BUILT";
#ifdef HAVE_PROFILER_TARGET
    {
        CUpti_Profiler_Initialize_Params ip = { CUpti_Profiler_Initialize_Params_STRUCT_SIZE };
        CUptiResult pinit = cupti_try(cuptiProfilerInitialize(&ip), "cuptiProfilerInitialize");

        CUptiResult probe = pinit;
        if (pinit == CUPTI_SUCCESS) {
            CUpti_Profiler_GetCounterAvailability_Params ga =
                { CUpti_Profiler_GetCounterAvailability_Params_STRUCT_SIZE };
            ga.ctx = ctx;
            probe = cupti_try(cuptiProfilerGetCounterAvailability(&ga),
                              "cuptiProfilerGetCounterAvailability");
        }

        if (probe == CUPTI_ERROR_INSUFFICIENT_PRIVILEGES)      t3a = "REFUSED_PRIVILEGES";
        else if (probe == CUPTI_SUCCESS)                       t3a = "ALLOWED";
        else if (probe == CUPTI_ERROR_NOT_SUPPORTED ||
                 probe == CUPTI_ERROR_NOT_COMPATIBLE)          t3a = "UNSUPPORTED_HERE";
        else                                                   t3a = "REFUSED_OTHER";
        printf("RESULT t3a_profiler_raw_status=%d\n", (int) probe);
        // Same rule as the other two: an operation this box cannot perform is
        // not evidence about the gate either way.
        if (strcmp(t3a, "UNSUPPORTED_HERE") != 0) t3_attempts++;
        if (probe == CUPTI_ERROR_INSUFFICIENT_PRIVILEGES) t3_refusals++;
    }
#else
    printf("  cupti_profiler_target.h not present at build time\n");
#endif
    printf("RESULT t3a_profiling_api=%s\n", t3a);

    // T3b — the strongest control available: gated ACTIVITY KINDS, reached
    // through cuptiActivityEnable, the very call T2 used to turn on kernel
    // timing. Same API, same process, same moment — the only thing that varies
    // is the KIND. If CONCURRENT_KERNEL is allowed while these are refused for
    // privileges, that IS the exemption in spec 5.4.4, demonstrated rather than
    // inferred. A different API refusing would leave the door open to it being
    // the API, not the gate, that differed.
    {
        struct GatedKind { CUpti_ActivityKind kind; const char* name; };
        const GatedKind gated[] = {
            { CUPTI_ACTIVITY_KIND_PC_SAMPLING,           "PC_SAMPLING" },
            { CUPTI_ACTIVITY_KIND_INSTRUCTION_EXECUTION, "INSTRUCTION_EXECUTION" },
        };
        for (const GatedKind& g : gated) {
            const CUptiResult r = cuptiActivityEnable(g.kind);
            const char* verd;
            if (r == CUPTI_ERROR_INSUFFICIENT_PRIVILEGES)   verd = "REFUSED_PRIVILEGES";
            else if (r == CUPTI_SUCCESS)                    verd = "ALLOWED";
            else if (r == CUPTI_ERROR_NOT_SUPPORTED ||
                     r == CUPTI_ERROR_NOT_COMPATIBLE)       verd = "UNSUPPORTED_HERE";
            else                                            verd = "REFUSED_OTHER";
            printf("RESULT t3b_activity_%s=%s\n", g.name, verd);
            printf("RESULT t3b_activity_%s_raw=%d\n", g.name, (int) r);
            if (r == CUPTI_SUCCESS) cuptiActivityDisable(g.kind);
            // An UNSUPPORTED kind answers nothing about the gate, so it must not
            // count as an attempt — that is how a blind probe reads as a pass.
            if (strcmp(verd, "UNSUPPORTED_HERE") != 0) t3_attempts++;
            if (r == CUPTI_ERROR_INSUFFICIENT_PRIVILEGES) t3_refusals++;
        }
    }

    // T3c — the legacy Events API. Independent of both paths above, and the
    // original subject of NVIDIA's restriction. Expected to be UNSUPPORTED on
    // compute capability 7.5 and newer (the 4090 included), which is why it is
    // a third opinion and not the deciding one.
    {
        CUpti_EventGroup group = nullptr;
        const CUptiResult r = cuptiEventGroupCreate(ctx, &group, 0);
        const char* verd;
        if (r == CUPTI_ERROR_INSUFFICIENT_PRIVILEGES)   verd = "REFUSED_PRIVILEGES";
        else if (r == CUPTI_SUCCESS)                    verd = "ALLOWED";
        else if (r == CUPTI_ERROR_NOT_SUPPORTED ||
                 r == CUPTI_ERROR_NOT_COMPATIBLE)       verd = "UNSUPPORTED_HERE";
        else                                            verd = "REFUSED_OTHER";
        printf("RESULT t3c_legacy_events=%s\n", verd);
        printf("RESULT t3c_legacy_events_raw=%d\n", (int) r);
        if (r == CUPTI_SUCCESS && group != nullptr) cuptiEventGroupDestroy(group);
        if (strcmp(verd, "UNSUPPORTED_HERE") != 0) t3_attempts++;
        if (r == CUPTI_ERROR_INSUFFICIENT_PRIVILEGES) t3_refusals++;
    }

    const int gate_active = t3_refusals > 0;
    printf("RESULT t3_gate_probes_answering=%d\n", t3_attempts);
    printf("RESULT t3_gate_refusals=%d\n", t3_refusals);
    printf("RESULT t3_gate_active=%s\n",
           gate_active ? "YES" : (t3_attempts > 0 ? "NO" : "UNKNOWN"));

    // ---- verdict -----------------------------------------------------------
    // The exemption is PROVEN only when the gate is demonstrably active, this
    // process is demonstrably unprivileged, and timing still works. Every other
    // combination is inconclusive and now says WHICH way it fell short, because
    // the two inconclusive cases need opposite remedies: one wants a humbler
    // account, the other wants the gate actually switched on.
    const char* verdict;
    if (!t1_ok || !t2_ok)
        verdict = "TIMING_BROKEN";
    else if (privileged == 1)
        verdict = "INCONCLUSIVE_PROCESS_PRIVILEGED";
    else if (gate_active)
        verdict = "EXEMPTION_PROVEN";           // timing works, gated ops refused
    else if (t3_attempts == 0)
        verdict = "INCONCLUSIVE_NO_GATE_PROBE";
    else
        verdict = "INCONCLUSIVE_GATE_NOT_ENFORCED";

    printf("\nRESULT verdict=%s\n", verdict);
    printf("\n");
    if (strcmp(verdict, "INCONCLUSIVE_PROCESS_PRIVILEGED") == 0) {
        printf("NOTE: this process holds the privilege the gate asks for, so\n"
               "      T1/T2 passing proves nothing about unprivileged users.\n"
               "      Re-run from a non-admin account (Windows) or a uid without\n"
               "      CAP_SYS_ADMIN (Linux), with the driver at its default.\n");
    } else if (strcmp(verdict, "INCONCLUSIVE_GATE_NOT_ENFORCED") == 0) {
        printf("NOTE: this process is UNPRIVILEGED and nothing the gate covers\n"
               "      was refused, so the gate is not being enforced on this box.\n"
               "      Timing working here therefore says nothing about a box\n"
               "      where it IS enforced. Turn the gate on explicitly and\n"
               "      re-run:  Windows  RmProfilingAdminOnly=1 under\n"
               "        HKLM\\SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\Global\\NVTweak\n"
               "               Linux    NVreg_RestrictProfilingToAdminUsers=1\n"
               "      Until then spec 5.4.4 stays an inference.\n");
    }

    cudaFree(d_out);
    cuCtxDestroy(ctx);
    return 0;
}
