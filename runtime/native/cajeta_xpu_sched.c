// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_xpu.c ===
// The scheduler seam (xpu-tile-scheduling §2): every launch arrives here as a
// submission, is recorded with its policy fields and the access sets bound
// from the kernel's manifest, and is dispatched through the raw launch ABI.
// Nothing reorders or defers yet; the record is what the later units read.

#define CAJ_SUB_MAX_HANDLES 16

struct caj_xpu_submission_rec {
    char name[256];
    int32_t gx, gy, gz, bx, by, bz;
    uint32_t sharedBytes;
    int64_t stream;
    int32_t policy;
    int32_t protectedBit;
    int32_t cuBudget;
    int32_t classified;
    int64_t earliestNs, latestNs, captureId, poolHandle, poolReserve;
    double energyJoules;
    int nRead, nWrite, nAcc;
    int64_t read[CAJ_SUB_MAX_HANDLES];
    int64_t write[CAJ_SUB_MAX_HANDLES];
    int64_t acc[CAJ_SUB_MAX_HANDLES];
};

static pthread_mutex_t g_xpu_sub_lock = PTHREAD_MUTEX_INITIALIZER;
static struct caj_xpu_submission_rec g_xpu_sub_last;
static int64_t g_xpu_sub_scheduled, g_xpu_sub_bypassed, g_xpu_sub_rejected;
static int g_xpu_sched_bypass = -1;   // -1 = the environment not read yet

// Names already reported as unclassified, so the diagnostic prints once each.
#define CAJ_SUB_MAX_NOTED 256
static char g_xpu_sub_noted[CAJ_SUB_MAX_NOTED][256];
static int g_xpu_sub_noted_count;

// The seam's own cost, on the profiler's instrumentation track when a profile
// is armed (§2.1a): calls and inclusive ns of the bookkeeping before dispatch.
static CajetaProfMethod caj_xpu_submit_span = {
    "cajeta.xpu.Scheduler", "submit", "cajeta_xpu_sched.c", 0, 0, 0,
    CAJ_INSTR_UNLINKED, 0, NULL
};

static int caj_xpu_sched_bypassed(void) {
    if (g_xpu_sched_bypass < 0) {
        const char* e = getenv("CAJETA_XPU_SCHEDULER");
        g_xpu_sched_bypass = (e && strcmp(e, "bypass") == 0) ? 1 : 0;
    }
    return g_xpu_sched_bypass;
}

void __cajeta_xpu_scheduler_bypass(int32_t on) { g_xpu_sched_bypass = on ? 1 : 0; }
int32_t __cajeta_xpu_scheduler_bypassed(void) { return caj_xpu_sched_bypassed(); }

int64_t __cajeta_xpu_submission_count(int32_t which) {
    int64_t v;
    pthread_mutex_lock(&g_xpu_sub_lock);
    v = which == 0 ? g_xpu_sub_scheduled
      : which == 1 ? g_xpu_sub_bypassed
      : which == 2 ? g_xpu_sub_rejected : 0;
    pthread_mutex_unlock(&g_xpu_sub_lock);
    return v;
}

static int64_t caj_xpu_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// The manifest JSON for (name, active backend), or NULL; caller holds no lock.
static const char* caj_xpu_manifest_text(const char* name, uint64_t* len) {
    int backend = cajeta_xpu_active_backend();
    const char* json = NULL;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    for (int i = 0; i < g_xpu_manifest_count; i++) {
        struct cajeta_xpu_manifest* m = &g_xpu_manifests[i];
        if (m->backend == backend && m->json &&
            strncmp(m->name, name, sizeof(m->name)) == 0) {
            json = m->json;
            *len = m->len;
            break;
        }
    }
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    return json;
}

// Reads the i-th "mode" under "access" into `out` (0 = read, 1 = write,
// 2 = readwrite, 3 = accumulate, -1 = other or absent).
static int caj_xpu_manifest_access_mode(const char* json, uint64_t len, int index) {
    static const char key[] = "\"access\"";
    const char* end = json + len;
    const char* p = NULL;
    for (uint64_t j = 0; j + sizeof(key) - 1 <= len; j++) {
        if (memcmp(json + j, key, sizeof(key) - 1) == 0) { p = json + j + sizeof(key) - 1; break; }
    }
    if (!p) return -1;
    while (p < end && *p != '[') p++;
    int depth = 0, entry = -1;
    for (; p < end; p++) {
        if (*p == '[') { depth++; continue; }
        if (*p == ']') { if (--depth == 0) return -1; continue; }
        if (*p == '{' && depth == 1) { entry++; continue; }
        if (entry != index) continue;
        if (strncmp(p, "\"mode\"", 6) != 0) continue;
        p += 6;
        while (p < end && (*p == ' ' || *p == ':')) p++;
        if (p >= end || *p != '"') return -1;
        p++;
        if (strncmp(p, "readwrite", 9) == 0) return 2;
        if (strncmp(p, "read", 4) == 0) return 0;
        if (strncmp(p, "write", 5) == 0) return 1;
        if (strncmp(p, "accumulate", 10) == 0) return 3;
        return -1;
    }
    return -1;
}

static void caj_xpu_sub_add(int64_t* set, int* n, int64_t h) {
    if (*n < CAJ_SUB_MAX_HANDLES) set[(*n)++] = h;
}

// A kernel's submission plan, cached by the launch site's name pointer: whether
// a manifest classifies it, and per argv slot the access mode of its buffer
// (-2 not a buffer, -1 a buffer with no mode). The manifest holds one access
// entry per KernelBuffer, Image or Texture parameter in declaration order
// (KernelAccess.cpp bufferParams), so the k-th such slot takes the k-th entry.
#define CAJ_SUB_PLAN_SLOTS 1024
#define CAJ_SUB_PLAN_PARAMS 64
struct caj_xpu_sub_plan {
    const char* name;
    int backend;
    uint32_t gen;
    int classified;
    int nparams;
    int8_t mode[CAJ_SUB_PLAN_PARAMS];
};
static struct caj_xpu_sub_plan g_xpu_sub_plans[CAJ_SUB_PLAN_SLOTS];

static void caj_xpu_sub_plan_fill(struct caj_xpu_sub_plan* p, const char* name) {
    uint64_t len = 0;
    const char* json = caj_xpu_manifest_text(name, &len);
    p->classified = json != NULL;
    p->nparams = 0;
    struct cajeta_kparams* kp = cajeta_xpu_find_kparams(name);
    if (!kp) return;
    int n = kp->count < CAJ_SUB_PLAN_PARAMS ? kp->count : CAJ_SUB_PLAN_PARAMS;
    int access = 0;
    for (int i = 0; i < n; i++) {
        uint8_t kind = kp->kind[i];
        if (kind != CAJETA_XPU_KP_BUFFER && kind != CAJETA_XPU_KP_TEXTURE &&
            kind != CAJETA_XPU_KP_IMAGE) {
            p->mode[i] = -2;
            continue;
        }
        p->mode[i] = json ? (int8_t) caj_xpu_manifest_access_mode(json, len, access) : -1;
        access++;
    }
    p->nparams = n;
}

// Copies the plan for `name` on the active backend into `out`, refilling the
// cached slot after any registration.
static void caj_xpu_sub_plan_for(const char* name, struct caj_xpu_sub_plan* out) {
    int backend = cajeta_xpu_active_backend();
    uint32_t gen = __atomic_load_n(&g_xpu_manifest_gen, __ATOMIC_ACQUIRE);
    size_t h = (size_t) (((uintptr_t) name >> 4) ^ (uintptr_t) backend) % CAJ_SUB_PLAN_SLOTS;
    pthread_mutex_lock(&g_xpu_sub_lock);
    struct caj_xpu_sub_plan* slot = NULL;
    for (int probe = 0; probe < 8; probe++) {
        struct caj_xpu_sub_plan* e = &g_xpu_sub_plans[(h + probe) % CAJ_SUB_PLAN_SLOTS];
        if ((e->name == name && e->backend == backend) || !e->name) { slot = e; break; }
    }
    if (!slot) slot = &g_xpu_sub_plans[h];
    if (slot->name != name || slot->backend != backend || slot->gen != gen) {
        pthread_mutex_unlock(&g_xpu_sub_lock);
        struct caj_xpu_sub_plan fresh;
        memset(&fresh, 0, sizeof fresh);
        fresh.name = name;
        fresh.backend = backend;
        fresh.gen = gen;
        caj_xpu_sub_plan_fill(&fresh, name);
        pthread_mutex_lock(&g_xpu_sub_lock);
        *slot = fresh;
    }
    *out = *slot;
    pthread_mutex_unlock(&g_xpu_sub_lock);
}

// Binds the access sets from the plan and the buffer handles in `argv`.
static void caj_xpu_sub_bind_sets(struct caj_xpu_submission_rec* r,
                                  const struct caj_xpu_sub_plan* p, void* argv) {
    if (!argv) return;
    void** av = (void**) argv;
    for (int i = 0; i < p->nparams; i++) {
        int mode = p->mode[i];
        if (mode < -1 || !av[i]) continue;
        int64_t h = *(const int64_t*) av[i];
        if (mode == 0 || mode == 2) caj_xpu_sub_add(r->read, &r->nRead, h);
        if (mode == 1 || mode == 2 || mode == 3) caj_xpu_sub_add(r->write, &r->nWrite, h);
        if (mode == 3) caj_xpu_sub_add(r->acc, &r->nAcc, h);
    }
}

static void caj_xpu_sub_note_unclassified(const char* name) {
    pthread_mutex_lock(&g_xpu_sub_lock);
    for (int i = 0; i < g_xpu_sub_noted_count; i++) {
        if (strncmp(g_xpu_sub_noted[i], name, 256) == 0) {
            pthread_mutex_unlock(&g_xpu_sub_lock);
            return;
        }
    }
    if (g_xpu_sub_noted_count < CAJ_SUB_MAX_NOTED) {
        strncpy(g_xpu_sub_noted[g_xpu_sub_noted_count], name, 255);
        g_xpu_sub_noted[g_xpu_sub_noted_count][255] = '\0';
        g_xpu_sub_noted_count++;
    }
    pthread_mutex_unlock(&g_xpu_sub_lock);
    fprintf(stderr,
            "cajeta.xpu: [xpu-unclassified] %s: no manifest for backend %s; "
            "scheduled as throughput with no access sets, measured from its first run\n",
            name, cajeta_xpu_backend_name(cajeta_xpu_active_backend()));
}

int32_t __cajeta_xpu_submit_v1(const char* kernelName,
                               int32_t gridX, int32_t gridY, int32_t gridZ,
                               int32_t blockX, int32_t blockY, int32_t blockZ,
                               uint32_t sharedBytes, void* argv,
                               int64_t streamHandle,
                               int32_t specCount, const int32_t* specValues,
                               const CajetaXpuSubmission* sub) {
    if (!kernelName) return 0;
    if (caj_xpu_sched_bypassed()) {
        pthread_mutex_lock(&g_xpu_sub_lock);
        g_xpu_sub_bypassed++;
        pthread_mutex_unlock(&g_xpu_sub_lock);
        __cajeta_xpu_launch_v3(kernelName, gridX, gridY, gridZ, blockX, blockY, blockZ,
                               sharedBytes, argv, streamHandle, -1, specCount, specValues);
        return 0;
    }
    int64_t span0 = __cajeta_prof_armed
        ? __cajeta_prof_instr_enter(&caj_xpu_submit_span) : 0;
    if (sub && sub->latestNs > 0 && caj_xpu_now_ns() > sub->latestNs) {
        pthread_mutex_lock(&g_xpu_sub_lock);
        g_xpu_sub_rejected++;
        pthread_mutex_unlock(&g_xpu_sub_lock);
        cajeta_xpu_clear_launch_refusal();
        cajeta_xpu_note_launch_refusal(kernelName, cajeta_xpu_active_backend());
        g_xpu_refusal_reason = 5;
        if (span0) __cajeta_prof_instr_exit(&caj_xpu_submit_span, span0);
        return 5;
    }

    struct caj_xpu_submission_rec r;
    memset(&r, 0, sizeof r);
    strncpy(r.name, kernelName, sizeof(r.name) - 1);
    r.gx = gridX; r.gy = gridY; r.gz = gridZ;
    r.bx = blockX; r.by = blockY; r.bz = blockZ;
    r.sharedBytes = sharedBytes;
    r.stream = streamHandle;
    r.policy = 0;
    if (sub) {
        if (sub->policy >= 0) r.policy = sub->policy;
        r.protectedBit = sub->protectedBit;
        r.cuBudget = sub->cuBudget;
        r.earliestNs = sub->earliestNs;
        r.latestNs = sub->latestNs;
        r.captureId = sub->captureId;
        r.energyJoules = sub->energyJoules;
        r.poolHandle = sub->poolHandle;
        r.poolReserve = sub->poolReserve;
    }
    if (r.policy == 1 || r.policy == 2) r.protectedBit = 1;   // §2.4
    struct caj_xpu_sub_plan plan;
    caj_xpu_sub_plan_for(kernelName, &plan);
    if (plan.classified) {
        r.classified = 1;
        caj_xpu_sub_bind_sets(&r, &plan, argv);
    } else {
        caj_xpu_sub_note_unclassified(kernelName);
    }
    pthread_mutex_lock(&g_xpu_sub_lock);
    g_xpu_sub_last = r;
    g_xpu_sub_scheduled++;
    pthread_mutex_unlock(&g_xpu_sub_lock);
    if (span0) __cajeta_prof_instr_exit(&caj_xpu_submit_span, span0);

    __cajeta_xpu_launch_v3(kernelName, gridX, gridY, gridZ, blockX, blockY, blockZ,
                           sharedBytes, argv, streamHandle, -1, specCount, specValues);
    return 0;
}

int64_t __cajeta_xpu_last_submission_field(int32_t key) {
    int64_t v = 0;
    pthread_mutex_lock(&g_xpu_sub_lock);
    const struct caj_xpu_submission_rec* r = &g_xpu_sub_last;
    switch (key) {
        case 0: v = r->gx; break;
        case 1: v = r->gy; break;
        case 2: v = r->gz; break;
        case 3: v = r->bx; break;
        case 4: v = r->by; break;
        case 5: v = r->bz; break;
        case 6: v = r->sharedBytes; break;
        case 7: v = r->stream; break;
        case 8: v = r->policy; break;
        case 9: v = r->protectedBit; break;
        case 10: v = r->earliestNs; break;
        case 11: v = r->latestNs; break;
        case 12: v = r->cuBudget; break;
        case 13: v = r->captureId; break;
        case 15: v = r->classified; break;
        case 16: v = r->nRead; break;
        case 17: v = r->nWrite; break;
        case 18: v = r->nAcc; break;
        case 19: v = r->poolHandle; break;
        case 20: v = r->poolReserve; break;
        default: break;
    }
    pthread_mutex_unlock(&g_xpu_sub_lock);
    return v;
}

double __cajeta_xpu_last_submission_energy(void) {
    double v;
    pthread_mutex_lock(&g_xpu_sub_lock);
    v = g_xpu_sub_last.energyJoules;
    pthread_mutex_unlock(&g_xpu_sub_lock);
    return v;
}

// STATIC native: `bufArr` is a cajeta int8[] whose payload starts at +8.
int64_t __cajeta_xpu_last_submission_name(void* bufArr, int64_t cap) {
    if (!bufArr || cap <= 0) return 0;
    pthread_mutex_lock(&g_xpu_sub_lock);
    int64_t n = (int64_t) strlen(g_xpu_sub_last.name);
    if (n > cap) n = cap;
    memcpy((char*) bufArr + 8, g_xpu_sub_last.name, (size_t) n);
    pthread_mutex_unlock(&g_xpu_sub_lock);
    return n;
}

int64_t __cajeta_xpu_last_submission_handle(int32_t set, int32_t i) {
    int64_t v = 0;
    pthread_mutex_lock(&g_xpu_sub_lock);
    const struct caj_xpu_submission_rec* r = &g_xpu_sub_last;
    if (i >= 0 && i < CAJ_SUB_MAX_HANDLES) {
        if (set == 0 && i < r->nRead) v = r->read[i];
        if (set == 1 && i < r->nWrite) v = r->write[i];
        if (set == 2 && i < r->nAcc) v = r->acc[i];
    }
    pthread_mutex_unlock(&g_xpu_sub_lock);
    return v;
}

void __cajeta_xpu_unregister_kernel_manifest(const char* kernelName, int32_t backend) {
    if (!kernelName) return;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    int w = 0;
    for (int i = 0; i < g_xpu_manifest_count; i++) {
        struct cajeta_xpu_manifest* m = &g_xpu_manifests[i];
        if (m->backend == backend && strncmp(m->name, kernelName, sizeof(m->name)) == 0)
            continue;
        if (w != i) g_xpu_manifests[w] = *m;
        w++;
    }
    g_xpu_manifest_count = w;
    __atomic_add_fetch(&g_xpu_manifest_gen, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_xpu_cuda_lock);
}
