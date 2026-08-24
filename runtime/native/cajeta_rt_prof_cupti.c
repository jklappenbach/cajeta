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
int32_t __cajeta_prof_cupti_version_is_wsl(const char* procVersion) {
    if (!procVersion) return 0;
    for (const char* p = procVersion; *p; ++p) {
        if ((p[0] == 'm' || p[0] == 'M') &&
            strncasecmp(p, "microsoft", 9) == 0)
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

void __cajeta_prof_cupti_reset(void) {
    pthread_mutex_lock(&caj_cupti_mutex);
    if (caj_cupti.lib) { caj_cupti_libclose(caj_cupti.lib); caj_cupti.lib = NULL; }
    caj_cupti.state = CAJETA_CUPTI_UNATTEMPTED;
    caj_cupti.bound = 0;
    caj_cupti.has_ts_callback = 0;
    memset(&caj_cupti.api, 0, sizeof(caj_cupti.api));
    caj_cupti.path[0] = '\0';
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
    void* lib = caj_cupti_load(tried, sizeof(tried));
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
            caj_cupti_libclose(lib);
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
    caj_cupti_say(CAJETA_CUPTI_READY, tried,
                  caj_cupti.has_ts_callback
                      ? "CUPTI bound (timestamp callback present)"
                      : "CUPTI bound (no timestamp callback; §6.9 conversion "
                        "path will apply)");
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
