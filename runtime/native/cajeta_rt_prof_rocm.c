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

#include <dlfcn.h>

#define CAJ_ROCM_PATH_MAX 512
#define CAJ_ROCM_REASON_MAX 256

typedef struct {
    void*   lib;
    int32_t state;
    char    path[CAJ_ROCM_PATH_MAX];     // what was tried, or what bound
    char    reason[CAJ_ROCM_REASON_MAX]; // why the state is what it is
} CajRocmState;

static CajRocmState caj_rocm = { NULL, CAJETA_ROCM_UNATTEMPTED, "", "" };
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
    caj_rocm.path[0] = '\0';
    caj_rocm.reason[0] = '\0';
    pthread_mutex_unlock(&caj_rocm_mutex);
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
    caj_rocm.lib = lib;
    caj_rocm_say(CAJETA_ROCM_READY, tried, "rocprofiler-sdk bound");
    pthread_mutex_unlock(&caj_rocm_mutex);
    return 1;
}

int32_t     __cajeta_prof_rocm_state(void)    { return caj_rocm.state; }
const char* __cajeta_prof_rocm_reason(void)   { return caj_rocm.reason; }
const char* __cajeta_prof_rocm_lib_path(void) { return caj_rocm.path; }

#endif  /* CAJETA_PROF_TRACE_STANDALONE */
