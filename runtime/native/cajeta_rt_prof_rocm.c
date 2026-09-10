// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c, before cajeta_rt_prof_gpu.c ===
// The ROCm (rocprofiler-sdk) device-timing backend: binding is a STATE carrying
// a reason, and a missing or late SDK degrades to host submit-to-complete.

#ifndef CAJETA_PROF_TRACE_STANDALONE

#define CAJ_ROCM_PATH_MAX 512
#define CAJ_ROCM_REASON_MAX 256

#if !defined(_WIN32)
#  include <dlfcn.h>

// ── The slice of the rocprofiler-sdk ABI this backend calls ──────────────
// Declared here rather than by including <rocprofiler-sdk/*.h>: the runtime is
// compiled to bitcode on machines with no ROCm, where a header dependency would
// make the absent-SDK path unbuildable exactly where it is needed.
typedef int32_t  caj_rocp_status_t;
typedef uint64_t caj_rocp_timestamp_t;
typedef uint64_t caj_rocp_thread_id_t;

typedef struct { uint64_t handle; } caj_rocp_context_id_t;
typedef struct { uint64_t handle; } caj_rocp_buffer_id_t;
typedef struct { uint64_t handle; } caj_rocp_callback_thread_t;
typedef union  { uint64_t value; void* ptr; } caj_rocp_user_data_t;

#define CAJ_ROCP_STATUS_SUCCESS 0
#define CAJ_ROCP_BUFFER_POLICY_LOSSLESS 2   // block rather than drop

typedef caj_rocp_status_t (*caj_rocp_force_configure_fn)(void*);
typedef caj_rocp_status_t (*caj_rocp_is_initialized_fn)(int*);
typedef caj_rocp_status_t (*caj_rocp_create_context_fn)(caj_rocp_context_id_t*);
typedef caj_rocp_status_t (*caj_rocp_start_context_fn)(caj_rocp_context_id_t);
typedef caj_rocp_status_t (*caj_rocp_stop_context_fn)(caj_rocp_context_id_t);
typedef caj_rocp_status_t (*caj_rocp_create_buffer_fn)(caj_rocp_context_id_t, size_t, size_t,
                                                      int32_t, void*, void*,
                                                      caj_rocp_buffer_id_t*);
typedef caj_rocp_status_t (*caj_rocp_flush_buffer_fn)(caj_rocp_buffer_id_t);
typedef caj_rocp_status_t (*caj_rocp_destroy_buffer_fn)(caj_rocp_buffer_id_t);
typedef caj_rocp_status_t (*caj_rocp_configure_buffer_tracing_fn)(caj_rocp_context_id_t, int32_t,
                                                                 const void*, size_t,
                                                                 caj_rocp_buffer_id_t);
typedef caj_rocp_status_t (*caj_rocp_push_external_fn)(caj_rocp_context_id_t, caj_rocp_thread_id_t,
                                                      caj_rocp_user_data_t);
typedef caj_rocp_status_t (*caj_rocp_pop_external_fn)(caj_rocp_context_id_t, caj_rocp_thread_id_t,
                                                     caj_rocp_user_data_t*);
typedef caj_rocp_status_t (*caj_rocp_get_thread_id_fn)(caj_rocp_thread_id_t*);
typedef caj_rocp_status_t (*caj_rocp_get_timestamp_fn)(caj_rocp_timestamp_t*);
typedef caj_rocp_status_t (*caj_rocp_create_callback_thread_fn)(caj_rocp_callback_thread_t*);
typedef caj_rocp_status_t (*caj_rocp_assign_callback_thread_fn)(caj_rocp_buffer_id_t,
                                                               caj_rocp_callback_thread_t);
typedef const char* (*caj_rocp_status_string_fn)(caj_rocp_status_t);
typedef int (*caj_rocp_kind_cb_fn)(int32_t kind, void* data);
typedef caj_rocp_status_t (*caj_rocp_iterate_kinds_fn)(caj_rocp_kind_cb_fn, void*);
typedef caj_rocp_status_t (*caj_rocp_kind_name_fn)(int32_t, const char**, uint64_t*);

// The two record shapes the buffer callback reads, leading fields only: the SDK
// appends to these, and `size` says how much of one this build may read.
typedef struct {
    uint32_t category;
    uint32_t kind;
    void*    payload;
} caj_rocp_record_header_t;

typedef struct {
    uint64_t size;
    int32_t  kind;
    int32_t  operation;
    struct { uint64_t internal; caj_rocp_user_data_t external; } correlation_id;
    uint64_t thread_id;
    uint64_t start_timestamp;   // device clock, CLOCK_BOOTTIME domain
    uint64_t end_timestamp;
    // dispatch_info follows; not read here.
} caj_rocp_kernel_dispatch_record_t;

#define CAJ_ROCM_KD_RECORD_MIN_SIZE ((uint64_t) sizeof(caj_rocp_kernel_dispatch_record_t))

// The tool-registration shapes. `size` is the SDK's own ABI guard: set to the
// sizeof this code understands, so a longer SDK struct keeps its tail to itself.
typedef struct { size_t size; const char* name; const uint32_t handle; } caj_rocp_client_id_t;
typedef int (*caj_rocp_tool_init_fn)(void* fini_func, void* tool_data);
typedef struct {
    size_t size;
    void*  initialize;   // caj_rocp_tool_init_fn
    void*  finalize;
    void*  tool_data;
} caj_rocp_tool_result_t;

typedef struct {
    caj_rocp_force_configure_fn          force_configure;
    caj_rocp_is_initialized_fn           is_initialized;
    caj_rocp_create_context_fn           create_context;
    caj_rocp_start_context_fn            start_context;
    caj_rocp_stop_context_fn             stop_context;
    caj_rocp_create_buffer_fn            create_buffer;
    caj_rocp_flush_buffer_fn             flush_buffer;
    caj_rocp_destroy_buffer_fn           destroy_buffer;
    caj_rocp_configure_buffer_tracing_fn configure_buffer_tracing;
    caj_rocp_push_external_fn            push_external;
    caj_rocp_pop_external_fn             pop_external;
    caj_rocp_get_thread_id_fn            get_thread_id;
    caj_rocp_get_timestamp_fn            get_timestamp;
    caj_rocp_create_callback_thread_fn   create_callback_thread;
    caj_rocp_assign_callback_thread_fn   assign_callback_thread;
    caj_rocp_status_string_fn            status_string;
    caj_rocp_iterate_kinds_fn            iterate_kinds;
    caj_rocp_kind_name_fn                kind_name;
} CajRocmApi;

// One row per entry point: the exported name and where its address goes. A table
// so the count the tests assert on is the same number binding walks.
typedef struct {
    const char* name;
    size_t      slot;      // byte offset into CajRocmApi
} CajRocmEntry;

#define CAJ_ROCM_ENTRY(field, sym) { sym, offsetof(CajRocmApi, field) }

static const CajRocmEntry caj_rocm_entries[] = {
    CAJ_ROCM_ENTRY(force_configure,          "rocprofiler_force_configure"),
    CAJ_ROCM_ENTRY(is_initialized,           "rocprofiler_is_initialized"),
    CAJ_ROCM_ENTRY(create_context,           "rocprofiler_create_context"),
    CAJ_ROCM_ENTRY(start_context,            "rocprofiler_start_context"),
    CAJ_ROCM_ENTRY(stop_context,             "rocprofiler_stop_context"),
    CAJ_ROCM_ENTRY(create_buffer,            "rocprofiler_create_buffer"),
    CAJ_ROCM_ENTRY(flush_buffer,             "rocprofiler_flush_buffer"),
    CAJ_ROCM_ENTRY(destroy_buffer,           "rocprofiler_destroy_buffer"),
    CAJ_ROCM_ENTRY(configure_buffer_tracing, "rocprofiler_configure_buffer_tracing_service"),
    CAJ_ROCM_ENTRY(push_external,            "rocprofiler_push_external_correlation_id"),
    CAJ_ROCM_ENTRY(pop_external,             "rocprofiler_pop_external_correlation_id"),
    CAJ_ROCM_ENTRY(get_thread_id,            "rocprofiler_get_thread_id"),
    CAJ_ROCM_ENTRY(get_timestamp,            "rocprofiler_get_timestamp"),
    CAJ_ROCM_ENTRY(create_callback_thread,   "rocprofiler_create_callback_thread"),
    CAJ_ROCM_ENTRY(assign_callback_thread,   "rocprofiler_assign_callback_thread"),
    CAJ_ROCM_ENTRY(status_string,            "rocprofiler_get_status_string"),
    CAJ_ROCM_ENTRY(iterate_kinds,            "rocprofiler_iterate_buffer_tracing_kinds"),
    CAJ_ROCM_ENTRY(kind_name,                "rocprofiler_query_buffer_tracing_kind_name"),
};

#define CAJ_ROCM_ENTRY_COUNT ((int32_t)(sizeof(caj_rocm_entries) / sizeof(caj_rocm_entries[0])))

typedef struct {
    void*      lib;
    int32_t    state;
    int32_t    bound;                    // entry points resolved by the last attempt
    int32_t    configured;               // force_configure succeeded IN THIS PROCESS
    int32_t    tool_init_ran;            // the SDK called back into us
    uint32_t   sdk_version;              // (10000*major)+(100*minor)+patch, from the callback
    // Written once inside tool_initialize, before any record can flow, and read
    // from the buffer callback WITHOUT the mutex — see caj_rocm_buffer_cb.
    caj_rocp_context_id_t ctx;
    caj_rocp_buffer_id_t  buf;
    int32_t    kd_kind;                  // discovered by name, never hardcoded
    int32_t    service_up;               // SDK context + buffer started; process-level
    int32_t    tracing;                  // this session is willing to use it
    int64_t    boot_minus_mono_ns;       // device->host clock mapping
    int64_t    records;                  // dispatch records seen
    int64_t    unmatched;                // records whose launch we never parked
    int64_t    launches;                 // launches offered to the SDK
    int32_t    offset_seen;              // an offset sample has been taken
    int64_t    offset_first;             // at trace start
    int64_t    offset_last;              // most recent
    int32_t    suspended;                // the gap moved mid-trace
    int64_t    suspend_ns;               // total movement attributed to suspends
    CajRocmApi api;
    char       path[CAJ_ROCM_PATH_MAX];     // what was tried, or what bound
    char       reason[CAJ_ROCM_REASON_MAX]; // why the state is what it is
} CajRocmState;

static CajRocmState caj_rocm;
static pthread_mutex_t caj_rocm_mutex = PTHREAD_MUTEX_INITIALIZER;

// Latch the backend state, the path tried and the reason; NULL keeps a field.
static void caj_rocm_say(int32_t state, const char* tried, const char* why) {
    caj_rocm.state = state;
    if (tried) { snprintf(caj_rocm.path, sizeof(caj_rocm.path), "%s", tried); }
    if (why)   { snprintf(caj_rocm.reason, sizeof(caj_rocm.reason), "%s", why); }
}

// dlopen one candidate, writing the path attempted into `out`.
static void* caj_rocm_try(const char* path, char* out, size_t outCap) {
    if (!path || !*path) return NULL;
    snprintf(out, outCap, "%s", path);
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

// Load librocprofiler-sdk, trying CAJETA_ROCPROF_LIB (an explicit override that
// is never fallen back from), then $ROCM_PATH/lib, /opt/rocm/lib, then the bare
// soname — the order HipDriver::loadHip uses, so both bind one ROCm install.
static void* caj_rocm_load(char* tried, size_t triedCap) {
    static const char* kSoname = "librocprofiler-sdk.so.1";
    char buf[CAJ_ROCM_PATH_MAX];

    const char* ov = getenv("CAJETA_ROCPROF_LIB");
    if (ov && *ov) {
        return caj_rocm_try(ov, tried, triedCap);
    }
    const char* rp = getenv("ROCM_PATH");
    if (rp && *rp) {
        void* h;
        snprintf(buf, sizeof(buf), "%s/lib/%s", rp, kSoname);
        h = caj_rocm_try(buf, tried, triedCap);
        if (h) return h;
    }
    {
        void* h;
        snprintf(buf, sizeof(buf), "/opt/rocm/lib/%s", kSoname);
        h = caj_rocm_try(buf, tried, triedCap);
        if (h) return h;
    }
    return caj_rocm_try(kSoname, tried, triedCap);
}

// Forget this module's binding and its counters. `configured` and the running
// service survive — process-wide, not undoable — so `tracing` follows the SDK.
void __cajeta_prof_rocm_reset(void) {
    pthread_mutex_lock(&caj_rocm_mutex);
    if (caj_rocm.lib) { dlclose(caj_rocm.lib); caj_rocm.lib = NULL; }
    caj_rocm.state = CAJETA_ROCM_UNATTEMPTED;
    caj_rocm.bound = 0;
    caj_rocm.records = 0;
    caj_rocm.unmatched = 0;
    caj_rocm.launches = 0;
    caj_rocm.offset_seen = 0;
    caj_rocm.suspended = 0;
    caj_rocm.suspend_ns = 0;
    caj_rocm.tracing = caj_rocm.service_up;
    memset(&caj_rocm.api, 0, sizeof(caj_rocm.api));
    caj_rocm.path[0] = '\0';
    caj_rocm.reason[0] = '\0';
    pthread_mutex_unlock(&caj_rocm_mutex);
}

// Resolve every entry point into caj_rocm.api; returns the index of the first
// symbol that did not resolve, or -1 when all did. All-or-nothing: a partial
// bind is only discovered mid-measurement, as a crash.
static int caj_rocm_bind(void* lib) {
    int i;
    for (i = 0; i < CAJ_ROCM_ENTRY_COUNT; ++i) {
        void* fn = dlsym(lib, caj_rocm_entries[i].name);
        if (!fn) return i;
        // The POSIX dlsym idiom casts through a data pointer, hence the offset.
        memcpy((char*)&caj_rocm.api + caj_rocm_entries[i].slot, &fn, sizeof(fn));
        caj_rocm.bound = i + 1;
    }
    return -1;
}

// Load and bind the SDK once, latching the outcome into the state machine.
// Returns 1 only for READY; a failure lands ABSENT, naming the first bad symbol.
int32_t __cajeta_prof_rocm_init(void) {
    pthread_mutex_lock(&caj_rocm_mutex);
    if (caj_rocm.state != CAJETA_ROCM_UNATTEMPTED) {
        const int32_t ready = (caj_rocm.state == CAJETA_ROCM_READY);
        pthread_mutex_unlock(&caj_rocm_mutex);
        return ready;
    }
    char tried[CAJ_ROCM_PATH_MAX];
    tried[0] = '\0';
    void* lib = caj_rocm_load(tried, sizeof(tried));
    if (!lib) {
        char why[CAJ_ROCM_REASON_MAX];
        const char* err = dlerror();
        snprintf(why, sizeof(why),
                 "rocprofiler-sdk not loadable (%s); GPU timing degrades to "
                 "host submit-to-complete",
                 err ? err : "no error reported by dlopen");
        caj_rocm_say(CAJETA_ROCM_ABSENT, tried, why);
        pthread_mutex_unlock(&caj_rocm_mutex);
        return 0;
    }
    caj_rocm.bound = 0;
    memset(&caj_rocm.api, 0, sizeof(caj_rocm.api));
    {
        const int missing = caj_rocm_bind(lib);
        if (missing >= 0) {
            char why[CAJ_ROCM_REASON_MAX];
            snprintf(why, sizeof(why),
                     "loaded %.128s but %s did not resolve (%d of %d entry "
                     "points bound); GPU timing degrades to host "
                     "submit-to-complete",
                     tried, caj_rocm_entries[missing].name,
                     caj_rocm.bound, CAJ_ROCM_ENTRY_COUNT);
            dlclose(lib);
            caj_rocm.bound = 0;
            memset(&caj_rocm.api, 0, sizeof(caj_rocm.api));
            caj_rocm_say(CAJETA_ROCM_ABSENT, tried, why);
            pthread_mutex_unlock(&caj_rocm_mutex);
            return 0;
        }
    }
    caj_rocm.lib = lib;
    caj_rocm_say(CAJETA_ROCM_READY, tried, "rocprofiler-sdk bound");
    pthread_mutex_unlock(&caj_rocm_mutex);
    return 1;
}

// ── Configuration, and the window it has to happen in ────────────────────

static caj_rocp_tool_result_t caj_rocm_tool_result;

// ── Buffered kernel-dispatch tracing ─────────────────────────────────────

// rocprofiler stamps dispatch records in the CLOCK_BOOTTIME domain while the host
// clock here is MONOTONIC; they differ by suspend time, so the gap is sampled.
#if defined(CLOCK_BOOTTIME)
#  define CAJ_CLOCK_BOOT CLOCK_BOOTTIME
#else
// No BOOTTIME: the offset comes out zero, which is true until a suspend.
#  define CAJ_CLOCK_BOOT CLOCK_MONOTONIC
#endif
// Clock `c` as nanoseconds; 0 when the clock cannot be read.
static int64_t caj_rocm_ns(clockid_t c) {
    struct timespec t;
    if (clock_gettime(c, &t) != 0) return 0;
    return (int64_t) t.tv_sec * 1000000000LL + (int64_t) t.tv_nsec;
}

// ── Suspend detection ────────────────────────────────────────────────────
#define CAJ_ROCM_SUSPEND_THRESHOLD_NS 1000000LL   // 1 ms; awake jitter is ~100 ns

// Feed one BOOTTIME-minus-MONOTONIC sample in: a move past the threshold is a
// suspend, latching `suspended` and returning 1. The first sample only seeds.
int32_t __cajeta_prof_rocm_note_clock_offset(int64_t offset) {
    int64_t delta;
    if (!caj_rocm.offset_seen) {
        caj_rocm.offset_seen  = 1;
        caj_rocm.offset_first = offset;
        caj_rocm.offset_last  = offset;
        return 0;
    }
    delta = offset - caj_rocm.offset_last;
    caj_rocm.offset_last = offset;
    if (delta > CAJ_ROCM_SUSPEND_THRESHOLD_NS || delta < -CAJ_ROCM_SUSPEND_THRESHOLD_NS) {
        caj_rocm.suspended = 1;
        caj_rocm.suspend_ns += delta;
        return 1;
    }
    return 0;
}

int32_t __cajeta_prof_rocm_suspended(void)  { return caj_rocm.suspended; }
int64_t __cajeta_prof_rocm_suspend_ns(void) { return caj_rocm.suspend_ns; }

// CLOCK_BOOTTIME minus CLOCK_MONOTONIC, monotonic read either side and the
// midpoint taken so the ~100 ns between reads does not bias the offset one way.
static int64_t caj_rocm_boot_minus_mono(void) {
    const int64_t m0 = caj_rocm_ns(CLOCK_MONOTONIC);
    const int64_t b  = caj_rocm_ns(CAJ_CLOCK_BOOT);
    const int64_t m1 = caj_rocm_ns(CLOCK_MONOTONIC);
    if (!m0 || !b || !m1) return 0;
    {
        static int32_t period_set = 0;
        if (!period_set)
            period_set = __cajeta_prof_clock_set_period(CAJ_GPU_BACKEND_HIP,
                                                        1.0);
        if (period_set) __cajeta_prof_clock_sample(CAJ_GPU_BACKEND_HIP, m0, b, m1);
    }
    return b - (m0 + (m1 - m0) / 2);
}

// iterate_kinds callback: latch the KERNEL_DISPATCH kind by NAME. A hardcoded
// enum value would silently subscribe to memory copies after an SDK insertion.
static int caj_rocm_find_kind_cb(int32_t kind, void* data) {
    const char* name = NULL;
    uint64_t    len  = 0;
    (void) data;
    if (caj_rocm.api.kind_name(kind, &name, &len) == CAJ_ROCP_STATUS_SUCCESS
            && name && strcmp(name, "KERNEL_DISPATCH") == 0)
        caj_rocm.kd_kind = kind;
    return 0;
}

// SDK buffer callback, on the SDK's own thread. Takes NO lock: flush_buffer may
// run it on a thread already holding caj_rocm_mutex, and all it reads is written
// once inside tool_initialize.
static void caj_rocm_buffer_cb(caj_rocp_context_id_t ctx, caj_rocp_buffer_id_t buf,
                               caj_rocp_record_header_t** headers, size_t count,
                               void* data, uint64_t drop_count) {
    size_t i;
    (void) ctx; (void) buf; (void) data; (void) drop_count;
    for (i = 0; i < count; ++i) {
        caj_rocp_record_header_t* h = headers ? headers[i] : NULL;
        caj_rocp_kernel_dispatch_record_t* r;
        if (!h || (int32_t) h->kind != caj_rocm.kd_kind || !h->payload) continue;
        r = (caj_rocp_kernel_dispatch_record_t*) h->payload;
        if (r->size < CAJ_ROCM_KD_RECORD_MIN_SIZE) continue;
        __atomic_add_fetch(&caj_rocm.records, 1, __ATOMIC_RELAXED);
        // A zero external id means the dispatch was not one of ours (a hipMemset
        // fill kernel, say); attributing it would invent a measurement.
        if (r->correlation_id.external.value == 0) {
            __atomic_add_fetch(&caj_rocm.unmatched, 1, __ATOMIC_RELAXED);
            continue;
        }
        if (!__cajeta_prof_gpu_resolve_dispatch(
                (int64_t) r->correlation_id.external.value,
                (int64_t) r->start_timestamp - caj_rocm.boot_minus_mono_ns,
                (int64_t) r->end_timestamp   - caj_rocm.boot_minus_mono_ns))
            __atomic_add_fetch(&caj_rocm.unmatched, 1, __ATOMIC_RELAXED);
    }
}

// Create the context and dispatch buffer and start tracing. Runs INSIDE
// force_configure with caj_rocm_mutex already held on this thread: must not lock.
static int caj_rocm_tool_initialize(void* fini_func, void* tool_data) {
    caj_rocp_status_t st;
    // The watermark must NOT be zero: rocprofiler-sdk 1.1.0 then delivers the
    // first record and reports the buffer as size 0, dropping everything after.
    const size_t kBufBytes  = 64u * 1024u;
    const size_t kWatermark = kBufBytes / 2u;

    (void) fini_func;
    (void) tool_data;
    caj_rocm.tool_init_ran = 1;
    caj_rocm.kd_kind = -1;

    st = caj_rocm.api.create_context(&caj_rocm.ctx);
    if (st != CAJ_ROCP_STATUS_SUCCESS) return 0;

    caj_rocm.api.iterate_kinds(caj_rocm_find_kind_cb, NULL);
    if (caj_rocm.kd_kind < 0) return 0;

    st = caj_rocm.api.create_buffer(caj_rocm.ctx, kBufBytes, kWatermark,
                                    CAJ_ROCP_BUFFER_POLICY_LOSSLESS,
                                    (void*) caj_rocm_buffer_cb, NULL, &caj_rocm.buf);
    if (st != CAJ_ROCP_STATUS_SUCCESS) return 0;

    // NULL operations + count 0 means every operation of the kind.
    st = caj_rocm.api.configure_buffer_tracing(caj_rocm.ctx, caj_rocm.kd_kind,
                                               NULL, 0, caj_rocm.buf);
    if (st != CAJ_ROCP_STATUS_SUCCESS) return 0;

    st = caj_rocm.api.start_context(caj_rocm.ctx);
    if (st != CAJ_ROCP_STATUS_SUCCESS) return 0;

    caj_rocm.boot_minus_mono_ns = caj_rocm_boot_minus_mono();
    __cajeta_prof_rocm_note_clock_offset(caj_rocm.boot_minus_mono_ns);
    caj_rocm.service_up = 1;
    caj_rocm.tracing = 1;
    // Non-zero would abort registration; a setup failure above leaves the tool
    // registered with `tracing` 0, which reads as "no device records".
    return 0;
}

// Push `launchId` as the SDK's external correlation id around the dispatch, which
// ties a device record back to its launch. Per-thread inside the SDK.
int32_t __cajeta_prof_rocm_push(int64_t launchId) {
    caj_rocp_user_data_t u;
    uint64_t tid = 0;
    // `tracing` outlives a reset; the api table does not, so check both.
    if (!caj_rocm.tracing || launchId <= 0) return 0;
    if (!caj_rocm.api.get_thread_id || !caj_rocm.api.push_external) return 0;
    if (caj_rocm.api.get_thread_id(&tid) != CAJ_ROCP_STATUS_SUCCESS) return 0;
    u.value = (uint64_t) launchId;
    __atomic_add_fetch(&caj_rocm.launches, 1, __ATOMIC_RELAXED);
    return caj_rocm.api.push_external(caj_rocm.ctx, tid, u) == CAJ_ROCP_STATUS_SUCCESS;
}

// Pop the external correlation id pushed around this thread's dispatch.
int32_t __cajeta_prof_rocm_pop(void) {
    caj_rocp_user_data_t back;
    uint64_t tid = 0;
    if (!caj_rocm.tracing) return 0;
    if (!caj_rocm.api.get_thread_id || !caj_rocm.api.pop_external) return 0;
    if (caj_rocm.api.get_thread_id(&tid) != CAJ_ROCP_STATUS_SUCCESS) return 0;
    back.value = 0;
    return caj_rocm.api.pop_external(caj_rocm.ctx, tid, &back) == CAJ_ROCP_STATUS_SUCCESS;
}

// ── The self-check ───────────────────────────────────────────────────────
// Sixteen, not one: a single launch can still be in flight at the first flush.
#define CAJ_ROCM_RECORD_CHECK_LAUNCHES 16

// Bound, configured and delivering nothing looks like success from the inside, so
// after that many launches with no record the device path is HARD disabled.
static void caj_rocm_check_records(void) {
    char why[CAJ_ROCM_REASON_MAX];
    if (!caj_rocm.tracing) return;
    if (__atomic_load_n(&caj_rocm.launches, __ATOMIC_ACQUIRE) < CAJ_ROCM_RECORD_CHECK_LAUNCHES)
        return;
    if (__atomic_load_n(&caj_rocm.records, __ATOMIC_ACQUIRE) > 0) return;

    caj_rocm.tracing = 0;
    snprintf(why, sizeof(why),
             "rocprofiler-sdk bound and configured but returned no dispatch "
             "records for %d launches; device timing disabled and degraded to "
             "host submit-to-complete. Usual causes: the driver is refusing "
             "profiling, the container lacks performance-counter access, or "
             "another rocprofiler tool already owns the dispatch service",
             CAJ_ROCM_RECORD_CHECK_LAUNCHES);
    caj_rocm_say(CAJETA_ROCM_NO_RECORDS, NULL, why);
}

// Drain whatever the SDK has buffered. Re-samples the clock mapping first, since
// a suspend since the last drain applies to none of the records it returns.
int32_t __cajeta_prof_rocm_flush(void) {
    if (!caj_rocm.tracing || !caj_rocm.api.flush_buffer) return 0;
    caj_rocm.boot_minus_mono_ns = caj_rocm_boot_minus_mono();
    __cajeta_prof_rocm_note_clock_offset(caj_rocm.boot_minus_mono_ns);
    {
        const int32_t ok = caj_rocm.api.flush_buffer(caj_rocm.buf) == CAJ_ROCP_STATUS_SUCCESS;
        // After the flush, not before: buffered records would disable a live one.
        caj_rocm_check_records();
        return ok;
    }
}

// The SDK's own clock through the same mapping every dispatch record gets, so
// the mapping can be checked directly rather than inferred from a record.
int64_t __cajeta_prof_rocm_device_now_ns(void) {
    uint64_t t = 0;
    if (!caj_rocm.api.get_timestamp) return 0;
    if (caj_rocm.api.get_timestamp(&t) != CAJ_ROCP_STATUS_SUCCESS) return 0;
    return (int64_t) t - caj_rocm.boot_minus_mono_ns;
}

int32_t __cajeta_prof_rocm_tracing(void)       { return caj_rocm.tracing; }
int32_t __cajeta_prof_rocm_dispatch_kind(void) { return caj_rocm.kd_kind; }
int64_t __cajeta_prof_rocm_records(void)   { return __atomic_load_n(&caj_rocm.records, __ATOMIC_ACQUIRE); }
int64_t __cajeta_prof_rocm_unmatched(void) { return __atomic_load_n(&caj_rocm.unmatched, __ATOMIC_ACQUIRE); }
int64_t __cajeta_prof_rocm_launches(void)  { return __atomic_load_n(&caj_rocm.launches, __ATOMIC_ACQUIRE); }
int32_t __cajeta_prof_rocm_record_threshold(void) { return CAJ_ROCM_RECORD_CHECK_LAUNCHES; }
int64_t __cajeta_prof_rocm_clock_offset_ns(void) { return caj_rocm.boot_minus_mono_ns; }

// The SDK's tool-registration entry, called from inside force_configure.
static caj_rocp_tool_result_t* caj_rocm_tool_configure(uint32_t version,
                                                      const char* runtime_version,
                                                      uint32_t priority,
                                                      caj_rocp_client_id_t* client_id) {
    (void) runtime_version;
    (void) priority;
    caj_rocm.sdk_version = version;
    if (client_id) client_id->name = "cajeta-profiler";
    caj_rocm_tool_result.size       = sizeof(caj_rocm_tool_result);
    caj_rocm_tool_result.initialize = (void*) caj_rocm_tool_initialize;
    caj_rocm_tool_result.finalize   = NULL;
    caj_rocm_tool_result.tool_data  = NULL;
    return &caj_rocm_tool_result;
}

// Configure rocprofiler, which must happen before HIP or HSA finish initializing
// — later is CAJETA_ROCM_LATE for good. Returns 1 once the process is configured.
int32_t __cajeta_prof_rocm_configure(void) {
    int already = 0;
    caj_rocp_status_t st;

    pthread_mutex_lock(&caj_rocm_mutex);
    if (caj_rocm.state != CAJETA_ROCM_READY) {
        pthread_mutex_unlock(&caj_rocm_mutex);
        return 0;
    }
    if (caj_rocm.configured) {
        caj_rocm.tracing = caj_rocm.service_up;
        pthread_mutex_unlock(&caj_rocm_mutex);
        return 1;
    }

    // is_initialized is a stable three-way answer where the "too late" status
    // code is not: 0 not yet, 1 done, -1 in progress — and -1 is just as closed.
    if (caj_rocm.api.is_initialized(&already) != CAJ_ROCP_STATUS_SUCCESS) already = 0;
    if (already != 0) {
        caj_rocm_say(CAJETA_ROCM_LATE, NULL,
                     already < 0
                         ? "rocprofiler was initializing before the profiler could "
                           "configure it; device timing degrades to host "
                           "submit-to-complete"
                         : "rocprofiler was already initialized before the profiler "
                           "could configure it — HIP or another ROCm tool started "
                           "first; device timing degrades to host submit-to-complete");
        pthread_mutex_unlock(&caj_rocm_mutex);
        return 0;
    }

    st = caj_rocm.api.force_configure((void*) caj_rocm_tool_configure);
    if (st != CAJ_ROCP_STATUS_SUCCESS) {
        char why[CAJ_ROCM_REASON_MAX];
        int now = 0;
        const char* text = caj_rocm.api.status_string ? caj_rocm.api.status_string(st) : NULL;
        if (caj_rocm.api.is_initialized(&now) != CAJ_ROCP_STATUS_SUCCESS) now = 0;
        if (now != 0) {
            snprintf(why, sizeof(why),
                     "rocprofiler finished initializing while the profiler was "
                     "configuring it (%s); device timing degrades to host "
                     "submit-to-complete",
                     text ? text : "no status text");
            caj_rocm_say(CAJETA_ROCM_LATE, NULL, why);
        } else {
            snprintf(why, sizeof(why),
                     "rocprofiler_force_configure failed (%s); device timing "
                     "degrades to host submit-to-complete",
                     text ? text : "no status text");
            caj_rocm_say(CAJETA_ROCM_ABSENT, NULL, why);
        }
        pthread_mutex_unlock(&caj_rocm_mutex);
        return 0;
    }

    caj_rocm.configured = 1;
    caj_rocm_say(CAJETA_ROCM_READY, NULL, "rocprofiler-sdk bound and configured");
    pthread_mutex_unlock(&caj_rocm_mutex);
    return 1;
}

int32_t     __cajeta_prof_rocm_state(void)         { return caj_rocm.state; }
const char* __cajeta_prof_rocm_reason(void)        { return caj_rocm.reason; }
const char* __cajeta_prof_rocm_lib_path(void)      { return caj_rocm.path; }
int32_t     __cajeta_prof_rocm_entry_count(void)   { return CAJ_ROCM_ENTRY_COUNT; }
int32_t     __cajeta_prof_rocm_entries_bound(void) { return caj_rocm.bound; }
int32_t     __cajeta_prof_rocm_configured(void)    { return caj_rocm.configured; }
int32_t     __cajeta_prof_rocm_tool_init_ran(void) { return caj_rocm.tool_init_ran; }

#else   /* _WIN32 */

// No ROCm on Windows and no dlfcn to look for it with. The state machine still
// answers ABSENT with a reason, so no caller has to ask what platform it is on.
static int32_t caj_rocm_win_state = CAJETA_ROCM_UNATTEMPTED;

void    __cajeta_prof_rocm_reset(void) { caj_rocm_win_state = CAJETA_ROCM_UNATTEMPTED; }
int32_t __cajeta_prof_rocm_init(void)  { caj_rocm_win_state = CAJETA_ROCM_ABSENT; return 0; }
int32_t __cajeta_prof_rocm_state(void) { return caj_rocm_win_state; }

const char* __cajeta_prof_rocm_reason(void) {
    return "rocprofiler-sdk is not available on Windows; GPU timing degrades "
           "to host submit-to-complete";
}
const char* __cajeta_prof_rocm_lib_path(void)      { return ""; }
int32_t     __cajeta_prof_rocm_entry_count(void)   { return 0; }
int32_t     __cajeta_prof_rocm_entries_bound(void) { return 0; }
int32_t     __cajeta_prof_rocm_configure(void)     { return 0; }
int32_t     __cajeta_prof_rocm_configured(void)    { return 0; }
int32_t     __cajeta_prof_rocm_tool_init_ran(void) { return 0; }
int32_t     __cajeta_prof_rocm_push(int64_t l)     { (void) l; return 0; }
int32_t     __cajeta_prof_rocm_pop(void)           { return 0; }
int32_t     __cajeta_prof_rocm_flush(void)         { return 0; }
int32_t     __cajeta_prof_rocm_tracing(void)       { return 0; }
int32_t     __cajeta_prof_rocm_dispatch_kind(void) { return -1; }
int64_t     __cajeta_prof_rocm_records(void)       { return 0; }
int64_t     __cajeta_prof_rocm_unmatched(void)     { return 0; }
int64_t     __cajeta_prof_rocm_clock_offset_ns(void) { return 0; }
int64_t     __cajeta_prof_rocm_device_now_ns(void)   { return 0; }
int64_t     __cajeta_prof_rocm_launches(void)        { return 0; }
int32_t     __cajeta_prof_rocm_record_threshold(void) { return 0; }
int32_t     __cajeta_prof_rocm_note_clock_offset(int64_t o) { (void) o; return 0; }
int32_t     __cajeta_prof_rocm_suspended(void)       { return 0; }
int64_t     __cajeta_prof_rocm_suspend_ns(void)      { return 0; }

#endif  /* !_WIN32 */

#endif  /* CAJETA_PROF_TRACE_STANDALONE */
