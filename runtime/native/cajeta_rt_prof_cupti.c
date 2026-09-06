// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
//
// cajeta-profiler Unit 12 — the CUPTI backend's loader and binding state
// (spec §5.4.1, §5.4.2, §10.5, §12.5).
//
// This is the Unit-8 pattern on NVIDIA: binding is a STATE, not a
// success/failure bit, every state carries an actionable sentence, and the
// entry points are declared here rather than by including <cupti.h> — the
// runtime compiles to bitcode on machines with no CUDA at all, and the ABSENT
// path is the one that matters most exactly there. Everything declared is a
// function pointer or a plain integer, stable across CUPTI versions; what is
// NOT stable is which symbols exist, and that is what binding checks.
//
// What deliberately does NOT live here yet: the Activity buffer machinery,
// external correlation, and the capability-ladder claims. The last of those
// is gated on Unit 1's §5.4.4 verdict (what an UNPRIVILEGED user may be
// promised); the loader is not, which is why it lands first and is exercised
// by the profiler-tests lanes on PHOENIX and phoenix-wsl.

#ifndef CAJETA_PROF_TRACE_STANDALONE

#define CAJ_CUPTI_PATH_MAX 512
#define CAJ_CUPTI_REASON_MAX 256

// The slice of the CUPTI ABI the backend calls. CUptiResult is a plain enum
// (0 = CUPTI_SUCCESS); handles are opaque pointers.
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
    // OPTIONAL — see the header note. CUDA 11.6+; absence selects §6.9's
    // conversion path rather than making the backend absent.
    caj_cupti_register_ts_cb_fn        register_timestamp_callback;
} CajCuptiApi;

typedef struct {
    const char* name;
    size_t      slot;
} CajCuptiEntry;

#define CAJ_CUPTI_ENTRY(field, sym) { sym, offsetof(CajCuptiApi, field) }

// The CORE set is all-or-nothing, for Unit 8's reason: a partial bind is only
// discovered mid-measurement, on the path that must degrade instead.
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
    // Unit 12's record path.
    int32_t    degraded;          // §5.4.3 — attached elsewhere; we are a no-op
    int32_t    ts_registered;     // the timestamp callback is in place
    int32_t    ts_status;         // the registration attempt's raw CUptiResult;
                                  // -1 = never attempted (symbol absent)
    int32_t    kinds_enabled;     // how many activity kinds we have enabled
    int32_t    configured;        // buffer callbacks registered + kinds enabled
    int32_t    ts_first;          // ts_registered happened at kinds_enabled == 0
    int64_t    records;           // kernel records decoded and usable
    int64_t    rejected;          // kernel records refused as unusable
    // The two halves of "a record arrived and still produced no span". A
    // kernel is only resolvable THROUGH an external-correlation record, so
    // counting both separates "the mapping records never came" from "they
    // came and did not match" — which are different bugs with different
    // fixes, and indistinguishable from `records` alone.
    int64_t    ext_records;       // external-correlation records noted
    int64_t    unmapped;          // kernel records with no mapping to a launch
    // Chokepoint ATTEMPTS, counted at entry before any early return. They
    // count how often the SEAM called, not how often CUPTI accepted — which
    // is the only way a test can tell "the launch path is wired to this" from
    // "these two functions work when called directly". 12.2.d was ticked
    // without that distinction and the wiring did not exist.
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

// ── §10.5 / §12.5 — WSL identification ───────────────────────────────────
//
// WSL is where run 32439821390 measured the timestamp callback ACCEPTED and
// then ignored — the records arrived in CUPTI's own domain with every call
// returning success. The platform therefore has to be identifiable so §6.9's
// detection is armed where the hazard lives. The kernel says so itself:
// /proc/version contains "microsoft" (WSL2 spells it lowercase inside
// "microsoft-standard-WSL2", WSL1 capitalized it), and no non-WSL kernel does.
// Hand-rolled rather than strncasecmp: the runtime is JIT-materialized, and
// one POSIX symbol the Windows host cannot resolve fails materialization of
// the WHOLE runtime — run 32776148357 took all 139 Windows tests down at
// 65–90 s apiece over exactly this call.
static int caj_cupti_imatch_microsoft(const char* p) {
    static const char kWord[9] = {'m','i','c','r','o','s','o','f','t'};
    for (int i = 0; i < 9; ++i) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != kWord[i]) return 0;
    }
    return 1;
}

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

// ── §5.4.2 — locate libcupti, honoring CUDA_HOME ─────────────────────────
//
// CUPTI does NOT live on the default loader path: it ships under
// extras/CUPTI/ inside a CUDA toolkit, which is why "dlopen the soname" alone
// finds nothing on a perfectly healthy install. Search order:
//
//   1. CAJETA_CUPTI_LIB — explicit override, honored and NOT fallen back
//      from (a typo must look like a typo, and the absent path must be
//      testable on machines that have CUPTI).
//   2. $CUDA_HOME/extras/CUPTI/lib64, then $CUDA_PATH's equivalent — the
//      documented toolkit layouts.
//   3. /usr/local/cuda/extras/CUPTI/lib64 — the default toolkit symlink.
//   4. The bare soname — whatever the loader resolves (covers distro
//      packages that DO place it on the path).

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

// Windows CUPTI ships a VERSIONED dll (cupti64_<year>.<n>.<n>.dll) under
// %CUDA_PATH%\extras\CUPTI\lib64, so the name cannot be spelled statically —
// the directory is globbed instead. The override remains a literal path.
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

// ── Unit 12 — the activity record path (spec §5.4.3, §5.4.5, §6.2) ───────
//
// RECORD LAYOUT IS MEASURED, NOT ASSUMED. Compiled against a real
// cupti_activity.h on 2026-08-30, offsetof() over every kernel record version
// it declares:
//
//   version   kind  start  end  correlationId  sizeof
//   Kernel2      0      8   16             84     112   <-- the odd one out
//   Kernel3      0     16   24             92     120
//   Kernel4      0     16   24             92     144
//   Kernel5      0     16   24             92     160
//   Kernel6      0     16   24             92     168
//   Kernel7      0     16   24             92     176
//   Kernel8      0     16   24             92     200
//   Kernel9      0     16   24             92     208
//
// The struct grows at the TAIL across eight versions while the prefix through
// correlationId does not move from Kernel3 on. That is the whole of §5.4.5:
// read the prefix by offset, never cast the record to a version-specific
// struct, and a toolkit shipping a newer record version cannot break parsing.
//
// Kernel2 (pre-CUDA 9) is the floor and WOULD misparse — its start sits where
// Kernel3 keeps padding. Nothing in a record identifies its version, so this
// cannot be detected directly; the plausibility rejections below are what stop
// a misparse from becoming a published measurement. A Kernel2 record decoded
// at Kernel3 offsets yields garbage that is overwhelmingly likely to be zero
// or inverted, which is exactly what they refuse.

#define CAJ_CUPTI_KIND_KERNEL             3   /* CUPTI_ACTIVITY_KIND_KERNEL */
#define CAJ_CUPTI_KIND_CONCURRENT_KERNEL 10   /* ..._CONCURRENT_KERNEL */
#define CAJ_CUPTI_KIND_EXTERNAL_CORRELATION 39
#define CAJ_CUPTI_KIND_DRIVER             4   /* ..._KIND_DRIVER */

#define CAJ_CUPTI_KREC_OFF_KIND   0
#define CAJ_CUPTI_KREC_OFF_START 16
#define CAJ_CUPTI_KREC_OFF_END   24
#define CAJ_CUPTI_KREC_OFF_CORR  92
#define CAJ_CUPTI_KREC_PREFIX   (CAJ_CUPTI_KREC_OFF_CORR + 4)   /* 96 */

/* CUPTI_ERROR_MULTIPLE_SUBSCRIBERS_NOT_SUPPORTED. Spelled as its value for the
 * same reason every other constant here is: this file must compile on a
 * machine with no CUDA at all. */
#define CAJ_CUPTI_ERR_MULTIPLE_SUBSCRIBERS 39

int32_t __cajeta_prof_cupti_kernel_prefix_bytes(void) {
    return CAJ_CUPTI_KREC_PREFIX;
}

// §12.1.b — CUPTI_ACTIVITY_KIND_KERNEL SERIALIZES kernel execution. Enabling
// it would change the program being measured rather than observe it, which is
// a different failure from being wrong: the numbers would be internally
// consistent and describe a program the user never ran. CONCURRENT_KERNEL is
// the only kernel kind this backend may enable.
int32_t __cajeta_prof_cupti_kind_is_allowed(int32_t kind) {
    // EXTERNAL_CORRELATION is not a kernel kind and does not serialize
    // anything: it emits one small record per push/pop, and pass 1 of the
    // buffer walk reads exactly those to learn which launch a kernel belongs
    // to. Refusing it would leave every kernel record unmapped, and an
    // unmapped kernel is DROPPED rather than guessed at (correctly) — so the
    // backend would deliver records, decode them, and publish no device span
    // at all, which is the hardest kind of nothing to debug.
    // DRIVER is admitted for one measured reason: CUPTI emits
    // EXTERNAL_CORRELATION records as part of the DRIVER/RUNTIME API activity
    // stream, not independently. With kind 39 enabled ALONE, CUPTI accepts the
    // enable, reports success, and produces no correlation record at all —
    // measured 2026-09-04, identically on Windows and WSL: kinds_enabled=2,
    // configure_rc=1, ext_correlation_records=0, and the one kernel record
    // dropped as unmapped. cajeta launches through the driver API
    // (cuLaunchKernel), so DRIVER is the stream its correlations ride.
    //
    // It costs a record per driver API call, which is real but bounded. It
    // does NOT serialize anything — that distinction is the whole reason
    // KERNEL stays refused while this is allowed.
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

    /* kind lives at offset 0 in every version, so it is readable before we
     * know anything else about the record. */
    if (!p || bytes < 4) return 0;
    if ((int32_t) caj_cupti_rd32(p + CAJ_CUPTI_KREC_OFF_KIND)
            != CAJ_CUPTI_KIND_CONCURRENT_KERNEL)
        return 0;

    /* §5.4.5 — a record that cannot hold the stable prefix is not decoded.
     * Reading past it would pick up whatever the buffer holds next, and the
     * result would look like a valid span. */
    if (bytes < CAJ_CUPTI_KREC_PREFIX) return 0;

    start = caj_cupti_rd64(p + CAJ_CUPTI_KREC_OFF_START);
    end   = caj_cupti_rd64(p + CAJ_CUPTI_KREC_OFF_END);

    /* §12.1.d — the known CUPTI regression, plus the shape a Kernel2 misparse
     * takes. Zero is not a time: publishing it would put a span at the epoch
     * and make every duration computed against it nonsense. Refused and
     * COUNTED, never clamped -- a clamp would republish the lie as plausible. */
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

// §5.4.3 — CUPTI permits exactly ONE subscriber per process. A program run
// under Nsight, or one that loads a second profiling library, hands us
// MULTIPLE_SUBSCRIBERS_NOT_SUPPORTED. Aborting there would take down a
// perfectly good program for the sake of a measurement it did not ask for, so
// the backend becomes a no-op and says why.
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

// §6.2 / §12.1.c — the callback must be in place BEFORE the first activity
// kind is enabled, or the records that arrive first are stamped in CUPTI's own
// domain and silently mixed with converted ones. Recorded as a fact about what
// happened rather than an intention: ts_first is set only if the registration
// landed while kinds_enabled was still zero.
static void caj_cupti_note_ts_registered(void) {
    caj_cupti.ts_registered = 1;
    if (caj_cupti.kinds_enabled == 0) caj_cupti.ts_first = 1;
}

int32_t __cajeta_prof_cupti_ts_callback_registered_first(void) {
    return caj_cupti.ts_first;
}

// 0 = registered, -1 = never attempted (symbol absent), >0 = the CUptiResult
// the driver refused with. The distinction decides whether §6.9's conversion
// path is a choice or a fallback, so it is reported rather than inferred.
int32_t __cajeta_prof_cupti_ts_status(void)     { return caj_cupti.ts_status; }
int32_t __cajeta_prof_cupti_ts_registered(void) { return caj_cupti.ts_registered; }

int64_t __cajeta_prof_cupti_records(void)  { return caj_cupti.records; }
int64_t __cajeta_prof_cupti_ext_records(void) { return caj_cupti.ext_records; }
int64_t __cajeta_prof_cupti_unmapped(void)    { return caj_cupti.unmapped; }
int64_t __cajeta_prof_cupti_rejected(void) { return caj_cupti.rejected; }
int64_t __cajeta_prof_cupti_pushes(void)   { return caj_cupti.pushes; }
int64_t __cajeta_prof_cupti_pops(void)     { return caj_cupti.pops; }


// ── 12.2.c — correlation, and why the parse is TWO passes ────────────────
//
// A kernel record does not carry our launch id. It carries CUPTI's own
// correlationId, and a SEPARATE record kind maps that to the external id we
// pushed at the launch chokepoint. So a buffer must be walked twice: pass one
// builds the map from EXTERNAL_CORRELATION records, pass two resolves kernels
// through it. One pass would drop every kernel whose mapping record happens to
// sit later in the same buffer, and that is not a rare ordering - CUPTI emits
// the correlation record when the range CLOSES, so it normally follows.
//
// MEASURED against the same real cupti_activity.h:
//   CUPTI_ACTIVITY_KIND_EXTERNAL_CORRELATION = 39
//   CUpti_ActivityExternalCorrelation:
//     kind=0  externalKind=4  externalId=8  correlationId=16  sizeof=24
// Read by offset for the same reason kernel records are.

#define CAJ_CUPTI_XREC_OFF_KIND    0
#define CAJ_CUPTI_XREC_OFF_EXT_ID  8
#define CAJ_CUPTI_XREC_OFF_CORR   16
#define CAJ_CUPTI_XREC_BYTES      24

/* CUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM0 - the slot NVIDIA reserves for tools
 * like this one, so pushing here cannot collide with a framework's own. */
#define CAJ_CUPTI_EXTERNAL_KIND_CUSTOM0 3

/* CUPTI_ERROR_MAX_LIMIT_REACHED - how GetNextRecord says "buffer exhausted".
 * It is the normal loop terminator, not a failure. */
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

// The map is FIXED SIZE and lives in static storage, because a buffer-complete
// callback runs on CUPTI's thread and must not allocate. 1024 entries covers a
// 64 KiB buffer many times over.
//
// On overflow it REFUSES and counts rather than evicting or wrapping. An
// evicted entry would turn into a lookup miss later, which is merely an
// unattributed kernel; a wrapped one would attribute a kernel to the WRONG
// launch, which is a plausible-looking measurement that is simply false. The
// first is a gap in the data and the second is a lie in it.
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

// ── 12.2.d — the launch chokepoint ───────────────────────────────────────
//
// Pushed before the launch and popped after, so every kernel CUPTI records
// between them carries our launch id. Called from the CUDA vtbl's begin/end in
// cajeta_rt_prof_gpu.c — and ONLY from there: the vtbl is selected only when
// tracing() is already true, exactly as the ROCm backend is, so a machine
// without CUDA runs the host lane and never reaches either of these. They stay
// defensive about it anyway, since both are reachable from a test.
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

// The two-pass walk itself. Reachable only with a bound CUPTI, so it is
// exercised on the PHOENIX and phoenix-wsl lanes rather than here; every piece
// it is built from is testable anywhere.
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
        /* An unmapped kernel is one we did not launch - a library's own, say.
         * Attributing it to any launch would invent a measurement. */
        if (!__cajeta_prof_cupti_corr_lookup(corr, &ext)) { caj_cupti.unmapped++; continue; }
        __cajeta_prof_gpu_resolve_dispatch(ext, start, end);
    }
}

// ── §6.8 / §12.2.e — the host clock, on both platforms ───────────────────
//
// CLOCK_MONOTONIC does not exist on the Windows host, and this file may NOT
// reach for a POSIX symbol it cannot resolve there: the runtime is
// JIT-materialized, and ONE unresolvable symbol fails materialization of the
// whole runtime. Run 32776148357 took all 139 Windows tests down at 65-90 s
// apiece over exactly that mistake, in this file.
//
// QPC is the Windows counterpart with the properties §6.8 needs: monotonic,
// not subject to NTP slew, and consistent across cores on any hardware this
// targets. The frequency is fixed at boot, so it is read once.
static int64_t caj_cupti_host_ns(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0 && !QueryPerformanceFrequency(&freq)) return 0;
    if (!QueryPerformanceCounter(&now)) return 0;
    /* Split to avoid overflowing the multiply: at 10 MHz a raw
     * ticks * 1e9 overflows int64 after about 29 years of uptime. */
    return (now.QuadPart / freq.QuadPart) * 1000000000LL
         + ((now.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (int64_t) t.tv_sec * 1000000000LL + (int64_t) t.tv_nsec;
#endif
}

// What CUPTI calls to stamp every activity record. Handing it OUR clock is
// what makes records arrive already in the host domain (§6.2) instead of
// needing §6.9's conversion after the fact.
static uint64_t caj_cupti_timestamp_cb(void) {
    return (uint64_t) caj_cupti_host_ns();
}

int64_t __cajeta_prof_cupti_host_ns(void) { return caj_cupti_host_ns(); }

// §12.1.b — the ONLY way this backend enables a kind. Routing every enable
// through the policy is what makes "KERNEL is never enabled" a property of
// the code rather than a promise about it.
int32_t __cajeta_prof_cupti_enable_kind(int32_t kind) {
    if (!__cajeta_prof_cupti_kind_is_allowed(kind)) return 0;
    if (caj_cupti.state != CAJETA_CUPTI_READY || caj_cupti.degraded) return 0;
    if (!caj_cupti.api.activity_enable) return 0;
    if (caj_cupti.api.activity_enable(kind) != 0) return 0;
    caj_cupti.kinds_enabled++;
    return 1;
}

int32_t __cajeta_prof_cupti_kinds_enabled(void) { return caj_cupti.kinds_enabled; }

// ── the arming step (the ROCm backend's __cajeta_prof_rocm_configure twin) ─
//
// Binding libcupti is NOT arming. Until the activity buffer callbacks are
// registered, `caj_cupti_consume_buffer` is unreachable and no record can ever
// be delivered; until a kind is enabled, none is ever produced. Both were
// missing, so `__cajeta_prof_cupti_tracing()` — which requires
// kinds_enabled > 0 — was false in every shipping build and every CUDA launch
// published at host tier no matter how completely CUPTI bound.
//
// The buffer size is a latency/throughput trade, not a correctness one: bigger
// means fewer completion callbacks and a longer wait before a record becomes
// visible to a flush. 1 MiB holds several thousand records.
#define CAJ_CUPTI_BUF_BYTES (1024 * 1024)

// CUPTI declares these with CUPTIAPI, which is __stdcall on Windows and empty
// elsewhere. On x86-64 Windows there is only one calling convention, so a
// plain function is ABI-identical — the timestamp callback above is already
// registered the same way and works on the PHOENIX lane.
static void caj_cupti_buffer_requested(uint8_t** buffer, size_t* size,
                                       size_t* maxNumRecords) {
    uint8_t* p = (uint8_t*) malloc(CAJ_CUPTI_BUF_BYTES);
    // malloc's alignment already satisfies the 8 bytes CUPTI requires of an
    // activity buffer on every 64-bit target the runtime builds for.
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

int32_t __cajeta_prof_cupti_configure(void) {
    int32_t okKernel, okExternal, okDriver;
    pthread_mutex_lock(&caj_cupti_mutex);
    if (caj_cupti.state != CAJETA_CUPTI_READY || caj_cupti.degraded) {
        // Nothing here can improve a backend that never bound, and calling
        // through the api table would be calling through nulls.
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
    // Callbacks BEFORE kinds: a record produced with nowhere to go is a record
    // lost, and the window between the two calls is real.
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

    // EXTERNAL_CORRELATION first: it is what a kernel record is resolved
    // THROUGH, so enabling the kernel kind first would open a window in which
    // kernels arrive that can never be attributed.
    // DRIVER first: it is the stream the correlation records ride, so enabling
    // it after them would open a window in which correlations are requested and
    // cannot be emitted.
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

// Drain CUPTI's completed activity buffers. Each buffer that comes back runs
// through caj_cupti_consume_buffer, which resolves parked launches; whatever
// is still unclaimed after this has waited long enough and drains at host
// tier. Safe when nothing bound.
int32_t __cajeta_prof_cupti_flush(void) {
    if (!__cajeta_prof_cupti_tracing()) return 0;
    if (!caj_cupti.api.activity_flush_all) return 0;
    return caj_cupti.api.activity_flush_all(0) == 0;
}

void __cajeta_prof_cupti_reset(void) {
    pthread_mutex_lock(&caj_cupti_mutex);
    // The library handle is deliberately NOT closed, and caj_cupti.path is
    // kept: once bound, libcupti is PINNED for the life of the process.
    //
    // CUPTI patches libcuda's driver dispatch table the first time any CUPTI
    // API runs — and register_timestamp_callback runs on every bind, WSL2's
    // refusal (CUptiResult 39) included: it initializes the interception
    // layer and THEN says no. dlclose would unmap the code those patched
    // pointers target while the pointers stay in the driver; the next cuInit
    // calls through the stale hook and the process dies with rip == the dead
    // address. That was the WSL SIGSEGV in
    // XpuDeviceProfileNvidiaDeviceTests.rawQueryAnswersOnCuda whenever any
    // CUPTI test ran earlier in the same binary (gdb: cuInit -> ?? at an
    // unmapped address, libcupti absent from `info sharedlibrary`). The old
    // comment here assumed the registration "belongs to the library handle";
    // it belongs to the driver, which we cannot unpatch. A later init reuses
    // caj_cupti.lib and rebinds. Every OTHER field resets below.
    caj_cupti.state = CAJETA_CUPTI_UNATTEMPTED;
    caj_cupti.bound = 0;
    caj_cupti.has_ts_callback = 0;
    caj_cupti.degraded = 0;
    caj_cupti.ts_registered = 0;
    caj_cupti.ts_status = -1;
    caj_cupti.kinds_enabled = 0;
    // A later init must configure again (cuptiActivityRegisterCallbacks is
    // re-registered on the still-loaded library; CUPTI replaces the pair).
    // NOT because the handle was closed — it wasn't (see above).
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
    // caj_cupti.path is kept: it names the pinned library, which is still
    // loaded — clearing it would report a mapping that is in fact present.
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

int32_t __cajeta_prof_cupti_init(void) {
    pthread_mutex_lock(&caj_cupti_mutex);
    if (caj_cupti.state != CAJETA_CUPTI_UNATTEMPTED) {
        const int32_t ready = (caj_cupti.state == CAJETA_CUPTI_READY);
        pthread_mutex_unlock(&caj_cupti_mutex);
        return ready;
    }
    char tried[CAJ_CUPTI_PATH_MAX];
    tried[0] = '\0';
    // A handle pinned by an earlier bind is REUSED, never re-dlopen'd (so the
    // refcount does not creep one per reset/init cycle) and never closed —
    // see __cajeta_prof_cupti_reset for why the mapping must outlive us.
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
            // Only a handle loaded by THIS call may be closed: no CUPTI API
            // has run on it yet, so the driver holds no pointers into it. A
            // pinned handle (a previous bind's) is left mapped — see reset.
            if (lib != caj_cupti.lib) caj_cupti_libclose(lib);
            caj_cupti.bound = 0;
            memset(&caj_cupti.api, 0, sizeof(caj_cupti.api));
            caj_cupti_say(CAJETA_CUPTI_ABSENT, tried, why);
            pthread_mutex_unlock(&caj_cupti_mutex);
            return 0;
        }
    }
    // The optional half, bound after the core set is certain (a CUPTI old
    // enough to lack it is still a working backend on §6.9's conversion path).
    {
        void* fn = caj_cupti_libsym(lib, "cuptiActivityRegisterTimestampCallback");
        caj_cupti.has_ts_callback = fn != NULL;
        if (fn) memcpy(&caj_cupti.api.register_timestamp_callback, &fn, sizeof(fn));
    }
    caj_cupti.lib = lib;
    // §6.2 / §12.2.b — register the timestamp callback HERE, before any
    // activity kind can be enabled. Order is the whole point: records that
    // arrive before the callback is in place are stamped in CUPTI's own
    // domain, and nothing downstream can tell them from converted ones.
    if (caj_cupti.has_ts_callback && caj_cupti.api.register_timestamp_callback) {
        caj_cupti.ts_status =
            (int32_t) caj_cupti.api.register_timestamp_callback(caj_cupti_timestamp_cb);
        if (caj_cupti.ts_status == 0) caj_cupti_note_ts_registered();
    }
    // THREE outcomes, not two. The symbol can be absent, present and accepted,
    // or present and REFUSED — and the third is real: phoenix-wsl resolves
    // cuptiActivityRegisterTimestampCallback and then rejects the registration
    // (measured 2026-08-30, run 33328180931). Discarding that status made a
    // platform difference look like an ordering bug, and left the operator
    // with a backend silently on the §6.9 conversion path and nothing saying
    // so. Binding is a STATE here and every state owes an actionable sentence.
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
