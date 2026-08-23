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
};

#define CAJ_ROCM_ENTRY_COUNT ((int32_t)(sizeof(caj_rocm_entries) / sizeof(caj_rocm_entries[0])))

typedef struct {
    void*      lib;
    int32_t    state;
    int32_t    bound;                    // entry points resolved by the last attempt
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
    memset(&caj_rocm.api, 0, sizeof(caj_rocm.api));
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

int32_t     __cajeta_prof_rocm_state(void)         { return caj_rocm.state; }
const char* __cajeta_prof_rocm_reason(void)        { return caj_rocm.reason; }
const char* __cajeta_prof_rocm_lib_path(void)      { return caj_rocm.path; }
int32_t     __cajeta_prof_rocm_entry_count(void)   { return CAJ_ROCM_ENTRY_COUNT; }
int32_t     __cajeta_prof_rocm_entries_bound(void) { return caj_rocm.bound; }

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

#endif  /* !_WIN32 */

#endif  /* CAJETA_PROF_TRACE_STANDALONE */
