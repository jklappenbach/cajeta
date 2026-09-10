// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
// The CUPTI backend's loader and binding state. Binding is a STATE, not a
// success bit; entry points are declared here so this compiles with no CUDA.

#ifndef CAJETA_PROF_TRACE_STANDALONE

#define CAJ_CUPTI_PATH_MAX 512
#define CAJ_CUPTI_REASON_MAX 256

// The slice of the CUPTI ABI the backend calls; handles are opaque pointers.
typedef int32_t caj_cupti_result_t;
#define CAJ_CUPTI_SUCCESS 0

typedef caj_cupti_result_t (*caj_cupti_subscribe_fn)(void**, void*, void*);
typedef caj_cupti_result_t (*caj_cupti_unsubscribe_fn)(void*);
typedef caj_cupti_result_t (*caj_cupti_activity_enable_fn)(int32_t);
typedef caj_cupti_result_t (*caj_cupti_activity_disable_fn)(int32_t);
typedef caj_cupti_result_t (*caj_cupti_activity_register_cbs_fn)(void*, void*);
typedef caj_cupti_result_t (*caj_cupti_activity_flush_all_fn)(uint32_t);
typedef caj_cupti_result_t (*caj_cupti_activity_next_record_fn)(uint8_t*, size_t,
                                                                void**);
typedef caj_cupti_result_t (*caj_cupti_push_external_fn)(int32_t, uint64_t);
typedef caj_cupti_result_t (*caj_cupti_pop_external_fn)(int32_t, uint64_t*);
typedef caj_cupti_result_t (*caj_cupti_get_timestamp_fn)(uint64_t*);
typedef caj_cupti_result_t (*caj_cupti_get_result_string_fn)(caj_cupti_result_t,
                                                             const char**);
typedef caj_cupti_result_t (*caj_cupti_register_ts_cb_fn)(uint64_t (*)(void));

typedef struct {
    caj_cupti_subscribe_fn             subscribe;
    caj_cupti_unsubscribe_fn           unsubscribe;
    caj_cupti_activity_enable_fn       activity_enable;
    caj_cupti_activity_disable_fn      activity_disable;
    caj_cupti_activity_register_cbs_fn activity_register_callbacks;
    caj_cupti_activity_flush_all_fn    activity_flush_all;
    caj_cupti_activity_next_record_fn  activity_get_next_record;
    caj_cupti_push_external_fn         push_external;
    caj_cupti_pop_external_fn          pop_external;
    caj_cupti_get_timestamp_fn         get_timestamp;
    caj_cupti_get_result_string_fn     get_result_string;
    // OPTIONAL: CUDA 11.6+. Absence selects the conversion path, not absence.
    caj_cupti_register_ts_cb_fn        register_timestamp_callback;
} CajCuptiApi;

typedef struct {
    const char* name;
    size_t      slot;
} CajCuptiEntry;

#define CAJ_CUPTI_ENTRY(field, sym) { sym, offsetof(CajCuptiApi, field) }

// The CORE set is all-or-nothing: a partial bind is discovered mid-measurement.
static const CajCuptiEntry caj_cupti_entries[] = {
    CAJ_CUPTI_ENTRY(subscribe,                   "cuptiSubscribe"),
    CAJ_CUPTI_ENTRY(unsubscribe,                 "cuptiUnsubscribe"),
    CAJ_CUPTI_ENTRY(activity_enable,             "cuptiActivityEnable"),
    CAJ_CUPTI_ENTRY(activity_disable,            "cuptiActivityDisable"),
    CAJ_CUPTI_ENTRY(activity_register_callbacks, "cuptiActivityRegisterCallbacks"),
    CAJ_CUPTI_ENTRY(activity_flush_all,          "cuptiActivityFlushAll"),
    CAJ_CUPTI_ENTRY(activity_get_next_record,    "cuptiActivityGetNextRecord"),
    CAJ_CUPTI_ENTRY(push_external,               "cuptiActivityPushExternalCorrelationId"),
    CAJ_CUPTI_ENTRY(pop_external,                "cuptiActivityPopExternalCorrelationId"),
    CAJ_CUPTI_ENTRY(get_timestamp,               "cuptiGetTimestamp"),
    CAJ_CUPTI_ENTRY(get_result_string,           "cuptiGetResultString"),
};

#define CAJ_CUPTI_ENTRY_COUNT \
    ((int32_t)(sizeof(caj_cupti_entries) / sizeof(caj_cupti_entries[0])))

typedef struct {
    void*      lib;
    int32_t    state;
    int32_t    bound;
    int32_t    has_ts_callback;
    int32_t    degraded;          // §5.4.3 — attached elsewhere; we are a no-op
    int32_t    ts_registered;     // the timestamp callback is in place
    int32_t    ts_status;         // the registration attempt's raw CUptiResult;
                                  // -1 = never attempted (symbol absent)
    int32_t    kinds_enabled;     // how many activity kinds we have enabled
    int32_t    configured;        // buffer callbacks registered + kinds enabled
    int32_t    ts_first;          // ts_registered happened at kinds_enabled == 0
    int64_t    records;           // kernel records decoded and usable
    int64_t    rejected;          // kernel records refused as unusable
    // "A record arrived and still produced no span" has two halves: mapping
    // records that never came, and records that came and did not match.
    int64_t    ext_records;       // external-correlation records noted
    int64_t    unmapped;          // kernel records with no mapping to a launch
    // Chokepoint ATTEMPTS: how often the SEAM called, not how often CUPTI took.
    int64_t    pushes;
    int64_t    pops;
    CajCuptiApi api;
    char       path[CAJ_CUPTI_PATH_MAX];
    char       reason[CAJ_CUPTI_REASON_MAX];
} CajCuptiState;

static CajCuptiState caj_cupti;
static pthread_mutex_t caj_cupti_mutex = PTHREAD_MUTEX_INITIALIZER;

static void caj_cupti_say(int32_t state, const char* tried, const char* why) {
    caj_cupti.state = state;
    if (tried) snprintf(caj_cupti.path, sizeof(caj_cupti.path), "%s", tried);
    if (why)   snprintf(caj_cupti.reason, sizeof(caj_cupti.reason), "%s", why);
}

// ── WSL identification ───────────────────────────────────────────────────
// WSL accepts the timestamp callback and then ignores it, so the platform must
// be identifiable: /proc/version says "microsoft". Matched by hand, because one
// unresolvable POSIX symbol fails materialization of the whole JIT runtime.
static int caj_cupti_imatch_microsoft(const char* p) {
    static const char kWord[9] = {'m','i','c','r','o','s','o','f','t'};
    for (int i = 0; i < 9; ++i) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != kWord[i]) return 0;
    }
    return 1;
}

// 1 when /proc/version's text names a WSL kernel. Takes the text, not the file,
// so the identification is testable on any platform.
int32_t __cajeta_prof_cupti_version_is_wsl(const char* procVersion) {
    if (!procVersion) return 0;
    for (const char* p = procVersion; *p; ++p) {
        if ((p[0] == 'm' || p[0] == 'M') && caj_cupti_imatch_microsoft(p))
            return 1;
    }
    return 0;
}

int32_t __cajeta_prof_cupti_on_wsl(void) {
#if defined(_WIN32)
    return 0;   // Windows proper is not WSL; the hazard is the Linux-on-WSL side
#else
    char buf[512];
    FILE* f = fopen("/proc/version", "rb");
    if (!f) return 0;
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    return __cajeta_prof_cupti_version_is_wsl(buf);
#endif
}

// ── locate libcupti, honoring CUDA_HOME ──────────────────────────────────
// CUPTI ships under extras/CUPTI/ inside a toolkit, not on the loader path, so
// the order is CAJETA_CUPTI_LIB (never fallen back from), $CUDA_HOME and
// $CUDA_PATH extras/CUPTI/lib64, /usr/local/cuda's, then the bare soname.

#if !defined(_WIN32)
#  include <dlfcn.h>

static void* caj_cupti_try(const char* path, char* out, size_t outCap) {
    if (!path || !*path) return NULL;
    snprintf(out, outCap, "%s", path);
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static void* caj_cupti_load(char* tried, size_t triedCap) {
    static const char* kSoname = "libcupti.so";
    char buf[CAJ_CUPTI_PATH_MAX];

    const char* ov = getenv("CAJETA_CUPTI_LIB");
    if (ov && *ov) return caj_cupti_try(ov, tried, triedCap);

    const char* roots[2] = { getenv("CUDA_HOME"), getenv("CUDA_PATH") };
    for (int i = 0; i < 2; ++i) {
        if (!roots[i] || !*roots[i]) continue;
        void* h;
        snprintf(buf, sizeof(buf), "%s/extras/CUPTI/lib64/%s", roots[i], kSoname);
        h = caj_cupti_try(buf, tried, triedCap);
        if (h) return h;
    }
    {
        void* h;
        snprintf(buf, sizeof(buf), "/usr/local/cuda/extras/CUPTI/lib64/%s", kSoname);
        h = caj_cupti_try(buf, tried, triedCap);
        if (h) return h;
    }
    return caj_cupti_try(kSoname, tried, triedCap);
}

#  define caj_cupti_libsym dlsym
#  define caj_cupti_libclose dlclose
#  define caj_cupti_liberr() dlerror()

#else  /* _WIN32 */
#  include <windows.h>

// Windows CUPTI ships a VERSIONED dll, so its directory is globbed by name.
static void* caj_cupti_try(const char* path, char* out, size_t outCap) {
    if (!path || !*path) return NULL;
    snprintf(out, outCap, "%s", path);
    return (void*) LoadLibraryA(path);
}

static void* caj_cupti_glob_dir(const char* dir, char* tried, size_t triedCap) {
    char pat[CAJ_CUPTI_PATH_MAX];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof(pat), "%s\\cupti64_*.dll", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(tried, triedCap, "%s", pat);
        return NULL;
    }
    char full[CAJ_CUPTI_PATH_MAX];
    snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
    FindClose(h);
    return caj_cupti_try(full, tried, triedCap);
}

static void* caj_cupti_load(char* tried, size_t triedCap) {
    const char* ov = getenv("CAJETA_CUPTI_LIB");
    if (ov && *ov) return caj_cupti_try(ov, tried, triedCap);

    const char* roots[2] = { getenv("CUDA_PATH"), getenv("CUDA_HOME") };
    for (int i = 0; i < 2; ++i) {
        if (!roots[i] || !*roots[i]) continue;
        char dir[CAJ_CUPTI_PATH_MAX];
        void* h;
        snprintf(dir, sizeof(dir), "%s\\extras\\CUPTI\\lib64", roots[i]);
        h = caj_cupti_glob_dir(dir, tried, triedCap);
        if (h) return h;
    }
    return caj_cupti_glob_dir(".", tried, triedCap);
}

static void* caj_cupti_libsym(void* lib, const char* n) {
    return (void*) GetProcAddress((HMODULE) lib, n);
}
static void caj_cupti_libclose(void* lib) { FreeLibrary((HMODULE) lib); }
static const char* caj_cupti_liberr(void) {
    static char msg[64];
    snprintf(msg, sizeof(msg), "GetLastError=%lu", (unsigned long) GetLastError());
    return msg;
}
#endif

// ── the activity record path ─────────────────────────────────────────────
// RECORD LAYOUT IS MEASURED: offsetof() over every kernel record version in
// cupti_activity.h, as version / kind / start / end / correlationId / sizeof,
//   Kernel2     0   8  16  84  112   <-- the odd one out
//   Kernel3..9  0  16  24  92  120..208
// so the prefix through correlationId is stable from Kernel3 on: read it by
// offset, never cast to a version. Kernel2 would misparse, and the
// plausibility rejections below are what stop that from being published.

#define CAJ_CUPTI_KIND_KERNEL             3   /* CUPTI_ACTIVITY_KIND_KERNEL */
#define CAJ_CUPTI_KIND_CONCURRENT_KERNEL 10   /* ..._CONCURRENT_KERNEL */
#define CAJ_CUPTI_KIND_EXTERNAL_CORRELATION 39
#define CAJ_CUPTI_KIND_DRIVER             4   /* ..._KIND_DRIVER */

#define CAJ_CUPTI_KREC_OFF_KIND   0
#define CAJ_CUPTI_KREC_OFF_START 16
#define CAJ_CUPTI_KREC_OFF_END   24
#define CAJ_CUPTI_KREC_OFF_CORR  92
#define CAJ_CUPTI_KREC_PREFIX   (CAJ_CUPTI_KREC_OFF_CORR + 4)   /* 96 */

/* CUPTI_ERROR_MULTIPLE_SUBSCRIBERS_NOT_SUPPORTED, spelled as its value: this
 * file must compile on a machine with no CUDA at all. */
#define CAJ_CUPTI_ERR_MULTIPLE_SUBSCRIBERS 39

int32_t __cajeta_prof_cupti_kernel_prefix_bytes(void) {
    return CAJ_CUPTI_KREC_PREFIX;
}

// KERNEL SERIALIZES execution; CONCURRENT_KERNEL is the only kind allowed.
int32_t __cajeta_prof_cupti_kind_is_allowed(int32_t kind) {
    // EXTERNAL_CORRELATION does not serialize and is what a kernel resolves
    // THROUGH; DRIVER is the stream those correlation records ride on.
    return kind == CAJ_CUPTI_KIND_CONCURRENT_KERNEL
        || kind == CAJ_CUPTI_KIND_EXTERNAL_CORRELATION
        || kind == CAJ_CUPTI_KIND_DRIVER;
}

static uint64_t caj_cupti_rd64(const unsigned char* p) {
    uint64_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static uint32_t caj_cupti_rd32(const unsigned char* p) {
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

// Returns  1 a usable CONCURRENT_KERNEL record (outputs written)
//          0 not a record this backend consumes (outputs untouched)
//         -1 a kernel record REFUSED as unusable (counted; outputs untouched)
int32_t __cajeta_prof_cupti_decode_kernel(const void* rec, int64_t bytes,
                                          int64_t* startNs, int64_t* endNs,
                                          int32_t* correlationId) {
    const unsigned char* p = (const unsigned char*) rec;
    uint64_t start, end;

    /* kind lives at offset 0 in every version. */
    if (!p || bytes < 4) return 0;
    if ((int32_t) caj_cupti_rd32(p + CAJ_CUPTI_KREC_OFF_KIND)
            != CAJ_CUPTI_KIND_CONCURRENT_KERNEL)
        return 0;

    /* Too small for the stable prefix: decoding would read the next record. */
    if (bytes < CAJ_CUPTI_KREC_PREFIX) return 0;

    start = caj_cupti_rd64(p + CAJ_CUPTI_KREC_OFF_START);
    end   = caj_cupti_rd64(p + CAJ_CUPTI_KREC_OFF_END);

    /* Zero is not a time, and is the shape a Kernel2 misparse takes: refused
     * and COUNTED, never clamped — a clamp republishes the lie as plausible. */
    if (start == 0 || end == 0 || end < start) {
        __atomic_add_fetch(&caj_cupti.rejected, 1, __ATOMIC_RELAXED);
        return -1;
    }

    if (startNs)       *startNs = (int64_t) start;
    if (endNs)         *endNs   = (int64_t) end;
    if (correlationId) *correlationId =
        (int32_t) caj_cupti_rd32(p + CAJ_CUPTI_KREC_OFF_CORR);
    __atomic_add_fetch(&caj_cupti.records, 1, __ATOMIC_RELAXED);
    return 1;
}

// CUPTI permits ONE subscriber per process: under Nsight the backend no-ops.
int32_t __cajeta_prof_cupti_note_subscribe_result(int32_t result) {
    if (result == CAJ_CUPTI_ERR_MULTIPLE_SUBSCRIBERS) {
        caj_cupti.degraded = 1;
        snprintf(caj_cupti.reason, sizeof(caj_cupti.reason),
                 "another CUPTI subscriber owns this process (Nsight, nvprof, "
                 "or a second profiling library); GPU timing degrades to host "
                 "submit-to-complete rather than failing the run");
        return 0;
    }
    if (result != 0) {
        caj_cupti.degraded = 1;
        snprintf(caj_cupti.reason, sizeof(caj_cupti.reason),
                 "cuptiSubscribe failed (%d); GPU timing degrades to host "
                 "submit-to-complete", (int) result);
        return 0;
    }
    caj_cupti.degraded = 0;
    return 1;
}

int32_t __cajeta_prof_cupti_degraded(void) { return caj_cupti.degraded; }

// ts_first records whether the callback landed while kinds_enabled was zero.
static void caj_cupti_note_ts_registered(void) {
    caj_cupti.ts_registered = 1;
    if (caj_cupti.kinds_enabled == 0) caj_cupti.ts_first = 1;
}

int32_t __cajeta_prof_cupti_ts_callback_registered_first(void) {
    return caj_cupti.ts_first;
}

// 0 = registered, -1 = never attempted (symbol absent), >0 = the refusing
// CUptiResult — which decides whether conversion is a choice or a fallback.
int32_t __cajeta_prof_cupti_ts_status(void)     { return caj_cupti.ts_status; }
int32_t __cajeta_prof_cupti_ts_registered(void) { return caj_cupti.ts_registered; }

int64_t __cajeta_prof_cupti_records(void)  { return caj_cupti.records; }
int64_t __cajeta_prof_cupti_ext_records(void) { return caj_cupti.ext_records; }
int64_t __cajeta_prof_cupti_unmapped(void)    { return caj_cupti.unmapped; }
int64_t __cajeta_prof_cupti_rejected(void) { return caj_cupti.rejected; }
int64_t __cajeta_prof_cupti_pushes(void)   { return caj_cupti.pushes; }
int64_t __cajeta_prof_cupti_pops(void)     { return caj_cupti.pops; }


// ── correlation, and why the parse is TWO passes ─────────────────────────
// A kernel record carries CUPTI's correlationId; a SEPARATE record maps it to
// our external id and is emitted when the range CLOSES, so it normally FOLLOWS
// its kernel. Measured: kind 39, kind=0 extKind=4 extId=8 corrId=16 sizeof=24.

#define CAJ_CUPTI_XREC_OFF_KIND    0
#define CAJ_CUPTI_XREC_OFF_EXT_ID  8
#define CAJ_CUPTI_XREC_OFF_CORR   16
#define CAJ_CUPTI_XREC_BYTES      24

/* CUSTOM0 — the slot NVIDIA reserves for tools, so a push cannot collide. */
#define CAJ_CUPTI_EXTERNAL_KIND_CUSTOM0 3

/* MAX_LIMIT_REACHED — GetNextRecord's "buffer exhausted"; a loop terminator. */
#define CAJ_CUPTI_ERR_MAX_LIMIT_REACHED 12

int32_t __cajeta_prof_cupti_decode_external(const void* rec, int64_t bytes,
                                            int32_t* correlationId,
                                            int64_t* externalId) {
    const unsigned char* p = (const unsigned char*) rec;
    if (!p || bytes < 4) return 0;
    if ((int32_t) caj_cupti_rd32(p + CAJ_CUPTI_XREC_OFF_KIND)
            != CAJ_CUPTI_KIND_EXTERNAL_CORRELATION)
        return 0;
    if (bytes < CAJ_CUPTI_XREC_BYTES) return 0;
    if (correlationId) *correlationId =
        (int32_t) caj_cupti_rd32(p + CAJ_CUPTI_XREC_OFF_CORR);
    if (externalId) *externalId =
        (int64_t) caj_cupti_rd64(p + CAJ_CUPTI_XREC_OFF_EXT_ID);
    return 1;
}

// FIXED SIZE in static storage: a buffer-complete callback runs on CUPTI's
// thread and must not allocate. Overflow refuses; wrapping would mis-attribute.
#define CAJ_CUPTI_CORR_CAP 1024

typedef struct {
    int32_t corr;      /* CUPTI's correlationId; 0 means the slot is free */
    int64_t external;  /* the launch id we pushed */
} CajCuptiCorr;

static CajCuptiCorr caj_cupti_corr[CAJ_CUPTI_CORR_CAP];
static int32_t      caj_cupti_corr_used;
static int64_t      caj_cupti_corr_dropped;

int32_t __cajeta_prof_cupti_corr_capacity(void) { return CAJ_CUPTI_CORR_CAP; }
int64_t __cajeta_prof_cupti_corr_dropped(void)  { return caj_cupti_corr_dropped; }

void __cajeta_prof_cupti_corr_reset(void) {
    memset(caj_cupti_corr, 0, sizeof(caj_cupti_corr));
    caj_cupti_corr_used = 0;
}

// Record one correlationId → launch id mapping; 0 when the fixed map is full.
int32_t __cajeta_prof_cupti_corr_note(int32_t correlationId, int64_t externalId) {
    if (correlationId == 0) return 0;      /* 0 is the free marker, not an id */
    if (caj_cupti_corr_used >= CAJ_CUPTI_CORR_CAP) {
        __atomic_add_fetch(&caj_cupti_corr_dropped, 1, __ATOMIC_RELAXED);
        return 0;
    }
    caj_cupti_corr[caj_cupti_corr_used].corr     = correlationId;
    caj_cupti_corr[caj_cupti_corr_used].external = externalId;
    caj_cupti_corr_used++;
    return 1;
}

int32_t __cajeta_prof_cupti_corr_lookup(int32_t correlationId, int64_t* externalId) {
    int32_t i;
    for (i = 0; i < caj_cupti_corr_used; ++i) {
        if (caj_cupti_corr[i].corr == correlationId) {
            if (externalId) *externalId = caj_cupti_corr[i].external;
            return 1;
        }
    }
    return 0;   /* a MISS leaves the output alone - see the test for why */
}

// ── the launch chokepoint ────────────────────────────────────────────────
// Pushed before the launch and popped after, so a kernel CUPTI records between
// them carries our launch id. Called only from the CUDA vtbl.
int32_t __cajeta_prof_cupti_tracing(void) {
    return caj_cupti.state == CAJETA_CUPTI_READY
        && !caj_cupti.degraded
        && caj_cupti.kinds_enabled > 0;
}

int32_t __cajeta_prof_cupti_push(int64_t launchId) {
    caj_cupti.pushes++;
    if (launchId == 0) return 0;           /* 0 is "no launch", not an id */
    if (!__cajeta_prof_cupti_tracing()) return 0;
    if (!caj_cupti.api.push_external) return 0;
    return caj_cupti.api.push_external(CAJ_CUPTI_EXTERNAL_KIND_CUSTOM0,
                                       (uint64_t) launchId) == 0;
}

int32_t __cajeta_prof_cupti_pop(void) {
    uint64_t popped = 0;
    caj_cupti.pops++;
    if (!__cajeta_prof_cupti_tracing()) return 0;
    if (!caj_cupti.api.pop_external) return 0;
    return caj_cupti.api.pop_external(CAJ_CUPTI_EXTERNAL_KIND_CUSTOM0,
                                      &popped) == 0;
}

// The two-pass walk itself; reachable only with a bound CUPTI.
static void caj_cupti_consume_buffer(uint8_t* buffer, size_t validSize) {
    void* rec;
    if (!caj_cupti.api.activity_get_next_record || validSize == 0) return;

    __cajeta_prof_cupti_corr_reset();

    /* pass 1 - the mapping records */
    rec = NULL;
    while (caj_cupti.api.activity_get_next_record(buffer, validSize, &rec) == 0) {
        int32_t corr = 0; int64_t ext = 0;
        if (__cajeta_prof_cupti_decode_external(rec, CAJ_CUPTI_XREC_BYTES,
                                                &corr, &ext) == 1) {
            __cajeta_prof_cupti_corr_note(corr, ext);
            caj_cupti.ext_records++;
        }
    }

    /* pass 2 - the kernels, resolved through the map built above */
    rec = NULL;
    while (caj_cupti.api.activity_get_next_record(buffer, validSize, &rec) == 0) {
        int64_t start = 0, end = 0, ext = 0;
        int32_t corr = 0;
        if (__cajeta_prof_cupti_decode_kernel(rec, CAJ_CUPTI_KREC_PREFIX,
                                              &start, &end, &corr) != 1)
            continue;
        /* An unmapped kernel is one we did not launch; attributing it invents. */
        if (!__cajeta_prof_cupti_corr_lookup(corr, &ext)) { caj_cupti.unmapped++; continue; }
        __cajeta_prof_gpu_resolve_dispatch(ext, start, end);
    }
}

// ── the host clock, on both platforms ────────────────────────────────────
// CLOCK_MONOTONIC does not exist on the Windows host, and ONE unresolvable
// POSIX symbol fails the whole JIT runtime. QPC is monotonic, NTP-proof, and
// its frequency is fixed at boot, so it is read once.
static int64_t caj_cupti_host_ns(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0 && !QueryPerformanceFrequency(&freq)) return 0;
    if (!QueryPerformanceCounter(&now)) return 0;
    /* Split to avoid overflowing the multiply: ticks * 1e9 overflows int64. */
    return (now.QuadPart / freq.QuadPart) * 1000000000LL
         + ((now.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (int64_t) t.tv_sec * 1000000000LL + (int64_t) t.tv_nsec;
#endif
}

// What CUPTI calls to stamp every record, so they arrive in the host domain.
static uint64_t caj_cupti_timestamp_cb(void) {
    return (uint64_t) caj_cupti_host_ns();
}

int64_t __cajeta_prof_cupti_host_ns(void) { return caj_cupti_host_ns(); }

// The ONLY way this backend enables a kind, so the KERNEL refusal is structural.
int32_t __cajeta_prof_cupti_enable_kind(int32_t kind) {
    if (!__cajeta_prof_cupti_kind_is_allowed(kind)) return 0;
    if (caj_cupti.state != CAJETA_CUPTI_READY || caj_cupti.degraded) return 0;
    if (!caj_cupti.api.activity_enable) return 0;
    if (caj_cupti.api.activity_enable(kind) != 0) return 0;
    caj_cupti.kinds_enabled++;
    return 1;
}

int32_t __cajeta_prof_cupti_kinds_enabled(void) { return caj_cupti.kinds_enabled; }

// ── the arming step ──────────────────────────────────────────────────────
// Binding libcupti is NOT arming: no record is delivered until the buffer
// callbacks are registered, and none is produced until a kind is enabled.
#define CAJ_CUPTI_BUF_BYTES (1024 * 1024)

// CUPTIAPI is __stdcall on Windows, and x86-64 Windows has one convention.
static void caj_cupti_buffer_requested(uint8_t** buffer, size_t* size,
                                       size_t* maxNumRecords) {
    uint8_t* p = (uint8_t*) malloc(CAJ_CUPTI_BUF_BYTES);
    // malloc's alignment already satisfies CUPTI's 8-byte requirement.
    *buffer = p;
    *size = p ? (size_t) CAJ_CUPTI_BUF_BYTES : 0;
    *maxNumRecords = 0;   /* as many as fit */
}

static void caj_cupti_buffer_completed(void* context, uint32_t streamId,
                                       uint8_t* buffer, size_t size,
                                       size_t validSize) {
    (void) context; (void) streamId; (void) size;
    caj_cupti_consume_buffer(buffer, validSize);
    free(buffer);
}

// Register the activity buffer callbacks and enable the allowed kinds. Returns
// 1 once the backend is actually tracing.
int32_t __cajeta_prof_cupti_configure(void) {
    int32_t okKernel, okExternal, okDriver;
    pthread_mutex_lock(&caj_cupti_mutex);
    if (caj_cupti.state != CAJETA_CUPTI_READY || caj_cupti.degraded) {
        // Nothing here can improve a backend that never bound.
        pthread_mutex_unlock(&caj_cupti_mutex);
        return 0;
    }
    if (caj_cupti.configured) {
        pthread_mutex_unlock(&caj_cupti_mutex);
        return caj_cupti.kinds_enabled > 0;
    }
    if (!caj_cupti.api.activity_register_callbacks) {
        caj_cupti_say(CAJETA_CUPTI_READY, NULL,
                      "CUPTI bound but cuptiActivityRegisterCallbacks did not "
                      "resolve; no activity record can be delivered and GPU "
                      "timing degrades to host submit-to-complete");
        pthread_mutex_unlock(&caj_cupti_mutex);
        return 0;
    }
    // Callbacks BEFORE kinds: a record produced with nowhere to go is lost.
    if (caj_cupti.api.activity_register_callbacks(
            (void*) caj_cupti_buffer_requested,
            (void*) caj_cupti_buffer_completed) != CAJ_CUPTI_SUCCESS) {
        caj_cupti_say(CAJETA_CUPTI_READY, NULL,
                      "cuptiActivityRegisterCallbacks failed; no activity "
                      "record can be delivered and GPU timing degrades to host "
                      "submit-to-complete");
        pthread_mutex_unlock(&caj_cupti_mutex);
        return 0;
    }
    caj_cupti.configured = 1;

    // EXTERNAL_CORRELATION and DRIVER first: a kernel resolves THROUGH one.
    okDriver   = __cajeta_prof_cupti_enable_kind(CAJ_CUPTI_KIND_DRIVER);
    okExternal = __cajeta_prof_cupti_enable_kind(CAJ_CUPTI_KIND_EXTERNAL_CORRELATION);
    okKernel   = __cajeta_prof_cupti_enable_kind(CAJ_CUPTI_KIND_CONCURRENT_KERNEL);
    if (!okKernel || !okExternal || !okDriver) {
        char why[CAJ_CUPTI_REASON_MAX];
        snprintf(why, sizeof(why),
                 "CUPTI bound but cuptiActivityEnable refused a kind it needs "
                 "(concurrent-kernel=%d external-correlation=%d driver=%d); %s, "
                 "so GPU timing degrades to host submit-to-complete",
                 (int) okKernel, (int) okExternal, (int) okDriver,
                 !okKernel ? "no kernel record will be produced"
                           : "kernel records cannot be tied to their launches");
        caj_cupti_say(CAJETA_CUPTI_READY, NULL, why);
        pthread_mutex_unlock(&caj_cupti_mutex);
        return 0;
    }
    caj_cupti_say(CAJETA_CUPTI_READY, NULL,
                  "CUPTI bound and configured (concurrent-kernel + "
                  "external-correlation + driver activity enabled; device "
                  "spans arrive in the host clock domain)");
    pthread_mutex_unlock(&caj_cupti_mutex);
    return 1;
}

int32_t __cajeta_prof_cupti_configured(void) { return caj_cupti.configured; }

// Drain CUPTI's completed buffers through caj_cupti_consume_buffer, which
// resolves the parked launches. Safe when nothing bound.
int32_t __cajeta_prof_cupti_flush(void) {
    if (!__cajeta_prof_cupti_tracing()) return 0;
    if (!caj_cupti.api.activity_flush_all) return 0;
    return caj_cupti.api.activity_flush_all(0) == 0;
}

void __cajeta_prof_cupti_reset(void) {
    pthread_mutex_lock(&caj_cupti_mutex);
    // The handle is deliberately NOT closed: CUPTI patches libcuda's dispatch
    // table on first use, so dlclose leaves the driver calling unmapped code.
    caj_cupti.state = CAJETA_CUPTI_UNATTEMPTED;
    caj_cupti.bound = 0;
    caj_cupti.has_ts_callback = 0;
    caj_cupti.degraded = 0;
    caj_cupti.ts_registered = 0;
    caj_cupti.ts_status = -1;
    caj_cupti.kinds_enabled = 0;
    // A later init must configure again; CUPTI replaces the callback pair.
    caj_cupti.configured = 0;
    caj_cupti.ts_first = 0;
    caj_cupti.records = 0;
    caj_cupti.ext_records = 0;
    caj_cupti.unmapped = 0;
    caj_cupti.rejected = 0;
    caj_cupti.pushes = 0;
    caj_cupti.pops = 0;
    __cajeta_prof_cupti_corr_reset();
    memset(&caj_cupti.api, 0, sizeof(caj_cupti.api));
    // caj_cupti.path is kept: it names the pinned library, still loaded.
    caj_cupti.reason[0] = '\0';
    pthread_mutex_unlock(&caj_cupti_mutex);
}

static int caj_cupti_bind(void* lib) {
    int i;
    for (i = 0; i < CAJ_CUPTI_ENTRY_COUNT; ++i) {
        void* fn = caj_cupti_libsym(lib, caj_cupti_entries[i].name);
        if (!fn) return i;
        memcpy((char*) &caj_cupti.api + caj_cupti_entries[i].slot, &fn, sizeof(fn));
        caj_cupti.bound = i + 1;
    }
    return -1;
}

// Load libcupti, bind the ABI slice, and settle the state with its reason.
int32_t __cajeta_prof_cupti_init(void) {
    pthread_mutex_lock(&caj_cupti_mutex);
    if (caj_cupti.state != CAJETA_CUPTI_UNATTEMPTED) {
        const int32_t ready = (caj_cupti.state == CAJETA_CUPTI_READY);
        pthread_mutex_unlock(&caj_cupti_mutex);
        return ready;
    }
    char tried[CAJ_CUPTI_PATH_MAX];
    tried[0] = '\0';
    // A pinned handle is REUSED, never re-dlopen'd and never closed — see reset.
    void* lib = caj_cupti.lib;
    if (lib) {
        snprintf(tried, sizeof(tried), "%s", caj_cupti.path);
    } else {
        lib = caj_cupti_load(tried, sizeof(tried));
    }
    if (!lib) {
        char why[CAJ_CUPTI_REASON_MAX];
        const char* err = caj_cupti_liberr();
        snprintf(why, sizeof(why),
                 "CUPTI not loadable (tried %.128s%s%s); GPU timing degrades "
                 "to host submit-to-complete",
                 tried[0] ? tried : "no candidate path",
                 err ? "; " : "", err ? err : "");
        caj_cupti_say(CAJETA_CUPTI_ABSENT, tried, why);
        pthread_mutex_unlock(&caj_cupti_mutex);
        return 0;
    }
    caj_cupti.bound = 0;
    memset(&caj_cupti.api, 0, sizeof(caj_cupti.api));
    {
        const int missing = caj_cupti_bind(lib);
        if (missing >= 0) {
            char why[CAJ_CUPTI_REASON_MAX];
            snprintf(why, sizeof(why),
                     "loaded %.128s but %s did not resolve (%d of %d entry "
                     "points bound); GPU timing degrades to host "
                     "submit-to-complete",
                     tried, caj_cupti_entries[missing].name,
                     caj_cupti.bound, CAJ_CUPTI_ENTRY_COUNT);
            // Only a handle loaded by THIS call may be closed: no CUPTI API ran.
            if (lib != caj_cupti.lib) caj_cupti_libclose(lib);
            caj_cupti.bound = 0;
            memset(&caj_cupti.api, 0, sizeof(caj_cupti.api));
            caj_cupti_say(CAJETA_CUPTI_ABSENT, tried, why);
            pthread_mutex_unlock(&caj_cupti_mutex);
            return 0;
        }
    }
    // The optional half, bound once the core set is certain.
    {
        void* fn = caj_cupti_libsym(lib, "cuptiActivityRegisterTimestampCallback");
        caj_cupti.has_ts_callback = fn != NULL;
        if (fn) memcpy(&caj_cupti.api.register_timestamp_callback, &fn, sizeof(fn));
    }
    caj_cupti.lib = lib;
    // Registered HERE, before any kind can be enabled: records arriving earlier
    // are stamped in CUPTI's own clock domain.
    if (caj_cupti.has_ts_callback && caj_cupti.api.register_timestamp_callback) {
        caj_cupti.ts_status =
            (int32_t) caj_cupti.api.register_timestamp_callback(caj_cupti_timestamp_cb);
        if (caj_cupti.ts_status == 0) caj_cupti_note_ts_registered();
    }
    // THREE outcomes: absent, accepted, or present and REFUSED (WSL2 does that).
    if (!caj_cupti.has_ts_callback) {
        caj_cupti_say(CAJETA_CUPTI_READY, tried,
                      "CUPTI bound (no cuptiActivityRegisterTimestampCallback in "
                      "this toolkit; §6.9 conversion path applies)");
    } else if (caj_cupti.ts_registered) {
        caj_cupti_say(CAJETA_CUPTI_READY, tried,
                      "CUPTI bound (timestamp callback registered; records "
                      "arrive in the host clock domain)");
    } else {
        char why[CAJ_CUPTI_REASON_MAX];
        snprintf(why, sizeof(why),
                 "CUPTI bound but the timestamp callback was REFUSED "
                 "(CUptiResult %d); records arrive in CUPTI's own clock domain "
                 "and §6.9 conversion applies. Known on WSL2.",
                 (int) caj_cupti.ts_status);
        caj_cupti_say(CAJETA_CUPTI_READY, tried, why);
    }
    pthread_mutex_unlock(&caj_cupti_mutex);
    return 1;
}


int32_t     __cajeta_prof_cupti_state(void)         { return caj_cupti.state; }
const char* __cajeta_prof_cupti_reason(void)        { return caj_cupti.reason; }
const char* __cajeta_prof_cupti_lib_path(void)      { return caj_cupti.path; }
int32_t     __cajeta_prof_cupti_entry_count(void)   { return CAJ_CUPTI_ENTRY_COUNT; }
int32_t     __cajeta_prof_cupti_entries_bound(void) { return caj_cupti.bound; }
int32_t     __cajeta_prof_cupti_has_timestamp_callback(void) {
    return caj_cupti.has_ts_callback;
}

#endif  /* CAJETA_PROF_TRACE_STANDALONE */
