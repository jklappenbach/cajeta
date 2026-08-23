// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
//
// cajeta-profiler Unit 8 — the ROCm backend (spec §5.2, §6.3).
//
// The requirement this file is built around is NOT "device timing works". It is
// §5.2.2: a run whose rocprofiler-sdk is missing must still succeed, with
// device timing degraded and the degradation reported. Degradation is never
// fatal (§10.4) — a host submit-to-complete window always exists, so there is
// always an honest measurement left to make. What must never happen is a rocm
// vtable that binds nothing and returns zeros, because a zero device span reads
// downstream exactly like a measured one.
//
// So binding is a STATE, not a success/failure bit, and every state carries a
// sentence a human can act on. See CAJETA_ROCM_* in cajeta_prof_abi.h for why
// the four are kept apart.
//
// Included BEFORE cajeta_rt_prof_gpu.c: that file's caj_gpu_vtbl_for() chooses
// between this backend and the host one, so this has to be defined first.

#ifndef CAJETA_PROF_TRACE_STANDALONE

#define CAJ_ROCM_PATH_MAX 512
#define CAJ_ROCM_REASON_MAX 256

#if !defined(_WIN32)
#  include <dlfcn.h>

// ── The slice of the rocprofiler-sdk ABI this backend calls ──────────────
//
// Declared here rather than by including <rocprofiler-sdk/*.h>. The runtime is
// compiled to bitcode on machines that have no ROCm at all, so a build-time
// dependency on the SDK headers would make the ABSENT path — the one §5.2.2 is
// about — unbuildable exactly where it is needed. Everything below is
// handle-shaped (a struct wrapping one uint64_t) or a plain integer, so the
// declarations are stable across SDK versions; what is NOT stable is which
// symbols exist, and that is what binding checks at runtime.
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

// The two record shapes the buffer callback reads. Only the leading fields are
// declared: the SDK appends to these structs, and `size` on every record says
// how much of it this build is entitled to read. Reading past what `size`
// promises is how a profiler starts reporting a future SDK's padding as
// timestamps, so the callback checks it.
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

// The tool-registration shapes. `size` is the SDK's own ABI guard: it is set to
// sizeof(the struct as this code understands it), so an SDK with a longer
// struct can tell that the trailing fields are not ours to read.
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

// One row per entry point: the exported name and where its address goes. Kept
// as a table so the count the tests assert on is the same number binding walks
// — a hand-maintained "expected count" constant would drift the moment a
// symbol is added, and drift in the direction of passing.
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
    // Buffered kernel-dispatch tracing (8.2.c). Written once inside
    // tool_initialize, before any record can flow, and read from the buffer
    // callback WITHOUT the mutex — see the deadlock note on caj_rocm_buffer_cb.
    caj_rocp_context_id_t ctx;
    caj_rocp_buffer_id_t  buf;
    int32_t    kd_kind;                  // discovered by name, never hardcoded
    int32_t    service_up;               // SDK context + buffer created and started;
                                         // process-level, since reset cannot undo it
    int32_t    tracing;                  // this session is willing to use it
    int64_t    boot_minus_mono_ns;       // §5.1.7 device->host clock mapping
    int64_t    records;                  // dispatch records seen (feeds §5.2.5)
    int64_t    unmatched;                // records whose launch we never parked
    int64_t    launches;                 // launches offered to the SDK (8.2.d)
    CajRocmApi api;
    char       path[CAJ_ROCM_PATH_MAX];     // what was tried, or what bound
    char       reason[CAJ_ROCM_REASON_MAX]; // why the state is what it is
} CajRocmState;

static CajRocmState caj_rocm;
static pthread_mutex_t caj_rocm_mutex = PTHREAD_MUTEX_INITIALIZER;

static void caj_rocm_say(int32_t state, const char* tried, const char* why) {
    caj_rocm.state = state;
    if (tried) { snprintf(caj_rocm.path, sizeof(caj_rocm.path), "%s", tried); }
    if (why)   { snprintf(caj_rocm.reason, sizeof(caj_rocm.reason), "%s", why); }
}

// Candidate search, in the order HipDriver::loadHip uses for libamdhip64 — the
// two must agree, or the profiler binds one ROCm install while the dispatches
// it is trying to time run against another.
//
//   1. CAJETA_ROCPROF_LIB, an explicit override. Its first purpose is testing:
//      §5.2.2's absent-SDK path is the one that matters most on machines with
//      no AMD hardware, and without an override it could only be exercised on
//      a machine that happens to lack the library. Its second is a deployment
//      escape hatch for a non-standard install.
//   2. $ROCM_PATH/lib — on this developer's box the whole ROCm tree lives
//      under ~/.local/lib/rocm and /opt/rocm holds only bin/, so the canonical
//      directory alone would find nothing.
//   3. /opt/rocm/lib, the update-alternatives target.
//   4. The bare soname, i.e. whatever ld.so resolves.
static void* caj_rocm_try(const char* path, char* out, size_t outCap) {
    if (!path || !*path) return NULL;
    snprintf(out, outCap, "%s", path);
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static void* caj_rocm_load(char* tried, size_t triedCap) {
    static const char* kSoname = "librocprofiler-sdk.so.1";
    char buf[CAJ_ROCM_PATH_MAX];

    const char* ov = getenv("CAJETA_ROCPROF_LIB");
    if (ov && *ov) {
        // An explicit override is honored and NOT fallen back from. Searching
        // on after it fails would make a typo look like a missing install, and
        // would let the absent-SDK test pass on a machine that has the SDK for
        // the wrong reason.
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

void __cajeta_prof_rocm_reset(void) {
    pthread_mutex_lock(&caj_rocm_mutex);
    if (caj_rocm.lib) { dlclose(caj_rocm.lib); caj_rocm.lib = NULL; }
    caj_rocm.state = CAJETA_ROCM_UNATTEMPTED;
    caj_rocm.bound = 0;
    // Counters are per-assessment and go back to zero: a NO_RECORDS verdict
    // from an earlier session must not outlive the session that reached it.
    caj_rocm.records = 0;
    caj_rocm.unmatched = 0;
    caj_rocm.launches = 0;
    // Willingness is restored to whatever the SDK is actually doing. Reset can
    // forget this module's verdict; it cannot stop a context the SDK has
    // already started, and claiming otherwise would leave the service running
    // with nobody reading it.
    caj_rocm.tracing = caj_rocm.service_up;
    memset(&caj_rocm.api, 0, sizeof(caj_rocm.api));
    // `configured` is deliberately NOT cleared. Which library is bound is this
    // module's business and can be forgotten; whether rocprofiler has been
    // configured belongs to the process and cannot be undone. Clearing it would
    // make a later configure attempt report a failure that never happened.
    caj_rocm.path[0] = '\0';
    caj_rocm.reason[0] = '\0';
    pthread_mutex_unlock(&caj_rocm_mutex);
}

// Resolve every entry point the backend calls, into caj_rocm.api. Returns the
// index of the first symbol that did not resolve, or -1 when all of them did.
//
// All-or-nothing on purpose. A partial bind would leave some slots null and the
// rest live, and the null ones are only discovered when a dispatch is being
// timed — i.e. mid-measurement, as a crash, on the path that §10.4 says must
// degrade instead. Refusing the whole library keeps the failure at init, where
// the fallback to the host window is still available.
static int caj_rocm_bind(void* lib) {
    int i;
    for (i = 0; i < CAJ_ROCM_ENTRY_COUNT; ++i) {
        void* fn = dlsym(lib, caj_rocm_entries[i].name);
        if (!fn) return i;
        // The cast through a data pointer is the POSIX-sanctioned dlsym idiom;
        // it is why this writes through a byte offset rather than assigning a
        // typed field directly.
        memcpy((char*)&caj_rocm.api + caj_rocm_entries[i].slot, &fn, sizeof(fn));
        caj_rocm.bound = i + 1;
    }
    return -1;
}

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
            // Name the symbol. "The SDK is too old" is only actionable if the
            // report says which entry point drew the version boundary, and a
            // library that loads but exports none of these is a different
            // mistake (wrong path) from one exporting most of them (wrong
            // version) — the name separates the two.
            snprintf(why, sizeof(why),
                     // The path is capped rather than allowed to eat the
                     // sentence: it is available in full from
                     // __cajeta_prof_rocm_lib_path(), and the symbol name is
                     // the part of this message that cannot be got elsewhere.
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

// ── 8.2.b — configuration, and the window it has to happen in ────────────
//
// rocprofiler intercepts HIP and HSA by installing itself into their dispatch
// tables while they load. Once the runtime it wants to intercept has finished
// initializing, that window is shut: force_configure returns
// CONFIGURATION_LOCKED and no dispatch will ever be traced. §5.2.3 is about
// exactly that — the failure is silent unless something checks, and a silently
// unconfigured profiler produces a trace with a GPU track full of host-tier
// spans that looks like a working device measurement.
//
// So configuration is a state transition like binding is, and missing the
// window lands in CAJETA_ROCM_LATE rather than in a log line nobody reads.

static caj_rocp_tool_result_t caj_rocm_tool_result;

// Called by the SDK from inside force_configure, synchronously. 8.2.c creates
// the context and the dispatch buffer here; for now it records that the SDK
// called back, which is the difference between "force_configure returned
// success" and "the SDK actually adopted us".
// ── 8.2.c — buffered kernel-dispatch tracing ─────────────────────────────

// CLOCK_BOOTTIME minus CLOCK_MONOTONIC, right now. rocprofiler stamps dispatch
// records in the BOOTTIME domain; CajetaGpuEvent.dev_*_ns is documented as
// already host-domain (§5.1.7), and the host clock here is MONOTONIC. The two
// differ by however long the machine has been suspended, which is zero on a
// server and minutes on a laptop that slept mid-trace — so this is sampled
// rather than assumed to be zero. §6.4's suspend detection (8.3.c) is the same
// quantity watched for a jump.
#if defined(CLOCK_BOOTTIME)
#  define CAJ_CLOCK_BOOT CLOCK_BOOTTIME
#else
// No BOOTTIME to compare against: the offset comes out zero, which is the truth
// right up until the machine suspends. Degrading to "no suspend correction" is
// better than refusing to map the clocks at all.
#  define CAJ_CLOCK_BOOT CLOCK_MONOTONIC
#endif
static int64_t caj_rocm_ns(clockid_t c) {
    struct timespec t;
    if (clock_gettime(c, &t) != 0) return 0;
    return (int64_t) t.tv_sec * 1000000000LL + (int64_t) t.tv_nsec;
}

static int64_t caj_rocm_boot_minus_mono(void) {
    // Monotonic is read either side of boottime and the midpoint taken. Two
    // sequential clock reads are ~100 ns apart, and reading them in a fixed
    // order puts that whole gap into the offset with a consistent sign — which
    // showed up immediately as a negative offset for a quantity that cannot be
    // negative. Bracketing cancels the bias instead of tolerating it.
    const int64_t m0 = caj_rocm_ns(CLOCK_MONOTONIC);
    const int64_t b  = caj_rocm_ns(CAJ_CLOCK_BOOT);
    const int64_t m1 = caj_rocm_ns(CLOCK_MONOTONIC);
    if (!m0 || !b || !m1) return 0;
    return b - (m0 + (m1 - m0) / 2);
}

// Find KERNEL_DISPATCH by NAME. Its enum value is 11 in this SDK, and writing
// 11 here would work until the SDK inserts a kind above it — at which point
// the profiler would quietly subscribe to memory copies and report them as
// kernels. The name is the stable identifier; the number is an implementation
// detail of the header this was not compiled against.
static int caj_rocm_find_kind_cb(int32_t kind, void* data) {
    const char* name = NULL;
    uint64_t    len  = 0;
    (void) data;
    if (caj_rocm.api.kind_name(kind, &name, &len) == CAJ_ROCP_STATUS_SUCCESS
            && name && strcmp(name, "KERNEL_DISPATCH") == 0)
        caj_rocm.kd_kind = kind;
    return 0;
}

// Called by the SDK on its own thread when the buffer reaches its watermark or
// is flushed.
//
// It takes NO lock. __cajeta_prof_rocm_flush() calls flush_buffer, and the SDK
// may run this synchronously on the calling thread; taking caj_rocm_mutex here
// would deadlock against a flush that already holds it. Everything read below
// is written once inside tool_initialize, before rocprofiler starts a context
// and therefore before any record can exist, so there is nothing to race with.
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
        // external is the launch id this profiler pushed. Zero means the
        // dispatch was not one of ours — a hipMemset fill kernel, say, which
        // this SDK does report. Attributing it to some launch would invent a
        // measurement, so it is counted and dropped.
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

// Runs INSIDE force_configure, on the calling thread, with caj_rocm_mutex
// already held by __cajeta_prof_rocm_configure. It must not lock.
static int caj_rocm_tool_initialize(void* fini_func, void* tool_data) {
    caj_rocp_status_t st;
    // 64 KiB holds ~350 dispatch records at this SDK's 184 bytes each, and the
    // watermark is half of it. The watermark must NOT be zero: measured against
    // librocprofiler-sdk 1.1.0, a zero watermark delivers the first record and
    // then reports the buffer as size 0, dropping everything after it.
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
    caj_rocm.service_up = 1;
    caj_rocm.tracing = 1;
    // Returning non-zero here aborts the SDK's registration. Setup failures
    // above return 0 too: the tool stays registered but `tracing` stays 0, and
    // the caller reads that as "no device records", which is the truth and is
    // already a state this backend knows how to degrade from.
    return 0;
}

// Push/pop the launch id as the SDK's external correlation id, around the
// dispatch. This is what ties a device record back to the launch that made it
// (§5.1.6) without a timestamp heuristic. Per-thread inside the SDK, so
// concurrent streams on different threads do not collide.
int32_t __cajeta_prof_rocm_push(int64_t launchId) {
    caj_rocp_user_data_t u;
    uint64_t tid = 0;
    // `tracing` can outlive a reset (the SDK is still running), but the api
    // table does not — reset clears it and init rebinds. Both have to be true.
    if (!caj_rocm.tracing || launchId <= 0) return 0;
    if (!caj_rocm.api.get_thread_id || !caj_rocm.api.push_external) return 0;
    if (caj_rocm.api.get_thread_id(&tid) != CAJ_ROCP_STATUS_SUCCESS) return 0;
    u.value = (uint64_t) launchId;
    __atomic_add_fetch(&caj_rocm.launches, 1, __ATOMIC_RELAXED);
    return caj_rocm.api.push_external(caj_rocm.ctx, tid, u) == CAJ_ROCP_STATUS_SUCCESS;
}

int32_t __cajeta_prof_rocm_pop(void) {
    caj_rocp_user_data_t back;
    uint64_t tid = 0;
    if (!caj_rocm.tracing) return 0;
    if (!caj_rocm.api.get_thread_id || !caj_rocm.api.pop_external) return 0;
    if (caj_rocm.api.get_thread_id(&tid) != CAJ_ROCP_STATUS_SUCCESS) return 0;
    back.value = 0;
    return caj_rocm.api.pop_external(caj_rocm.ctx, tid, &back) == CAJ_ROCP_STATUS_SUCCESS;
}

// ── 8.2.d — the self-check (§5.2.5) ──────────────────────────────────────
//
// Bound, configured, and delivering nothing is a real state, and it is the one
// that looks most like success from the inside: every call returned SUCCESS.
// It happens when the driver refuses profiling, when a container lacks the
// performance-counter capability, or when another rocprofiler tool already owns
// the dispatch service. Left alone, the profiler parks every launch, publishes
// them all at host tier, and reports itself as a working device backend.
//
// So after enough launches have gone by with a flush and still no record, the
// device path is HARD DISABLED: tracing off, state NO_RECORDS, and the vtable
// selector — which requires READY — drops backend 1 back to the host lane. The
// point of disabling rather than merely reporting is that the parking overhead
// buys nothing once it is known that no record will ever claim a parked launch.
//
// Sixteen, not one: a single launch can legitimately still be in flight when
// the first flush happens, and disabling on that would turn a timing race into
// a permanent downgrade.
#define CAJ_ROCM_RECORD_CHECK_LAUNCHES 16

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

// Drain whatever the SDK has buffered. Re-samples the clock mapping first:
// records completed since the last drain, and a suspend inside that window
// would otherwise be applied to none of them.
int32_t __cajeta_prof_rocm_flush(void) {
    if (!caj_rocm.tracing || !caj_rocm.api.flush_buffer) return 0;
    caj_rocm.boot_minus_mono_ns = caj_rocm_boot_minus_mono();
    {
        const int32_t ok = caj_rocm.api.flush_buffer(caj_rocm.buf) == CAJ_ROCP_STATUS_SUCCESS;
        // After the flush, not before: the check asks whether anything HAS come
        // back, and asking with records still sitting in the SDK's buffer would
        // disable a backend that was working.
        caj_rocm_check_records();
        return ok;
    }
}

// The SDK's own clock, mapped into the host domain — the same conversion every
// dispatch record goes through, exposed so the mapping can be checked directly
// rather than inferred from a record's plausibility.
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

int32_t __cajeta_prof_rocm_configure(void) {
    int already = 0;
    caj_rocp_status_t st;

    pthread_mutex_lock(&caj_rocm_mutex);
    if (caj_rocm.state != CAJETA_ROCM_READY) {
        // ABSENT stays ABSENT and LATE stays LATE. Nothing here can improve
        // either, and calling through the api table would be calling through
        // nulls.
        pthread_mutex_unlock(&caj_rocm_mutex);
        return 0;
    }
    if (caj_rocm.configured) {
        // The service the first configure started is still running, so this
        // session may use it.
        caj_rocm.tracing = caj_rocm.service_up;
        // Configuration is irreversible and process-wide, so having done it
        // once is success, not lateness. This is why `configured` survives
        // __cajeta_prof_rocm_reset(): reset can forget which library was bound,
        // but it cannot un-configure the SDK, and claiming otherwise would make
        // the second call report a failure that did not happen.
        pthread_mutex_unlock(&caj_rocm_mutex);
        return 1;
    }

    // Ask before acting. The status code for "too late" moves position in the
    // SDK's error enum as codes are added, and hardcoding its current value
    // would make this misread a future SDK's unrelated error as lateness.
    // is_initialized is a stable three-way answer: 0 not yet, 1 done,
    // -1 in progress — and -1 is just as closed a window as 1.
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
        // The window can shut between the question and the call — another
        // thread bringing HIP up is enough. Re-asking separates that race from
        // an unrelated configuration error, so the report names the right one.
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
// answers, because callers must not have to ask what platform they are on to
// know whether device timing is available — ABSENT with a reason is the same
// answer a Linux box without the SDK gives, and the host window still works.
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

#endif  /* !_WIN32 */

#endif  /* CAJETA_PROF_TRACE_STANDALONE */
