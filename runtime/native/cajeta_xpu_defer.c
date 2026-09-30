// === cajeta_xpu_defer.c — the launch resolution cache, deferred launches and
// their replay. #included into cajeta_xpu.c ahead of cajeta_xpu_launch.c.
//
// Two costs sit under every launch and neither depends on the kernel.
//
// RESOLUTION. A launch names its kernel by string, and the runtime used to
// answer six questions about that name with six linear scans of 256-byte name
// slots (kernel params three times, the OptiX program, the module, the
// relocated shared block), a seventh in the census, and a driver symbol lookup
// for the specialization globals nearly no module has. `caj_kres_get` answers
// all of them once per launch-site name and again only when a registration
// moves.
//
// SUBMISSION. The driver call itself: 6 to 13 microseconds a launch on the
// measured WSL2 box for a 64-element kernel, against about 0.3 microseconds a
// kernel inside a replayed CUDA graph plus 6 for the graph. A stream marked
// deferred queues its launches (arguments copied by value) and submits them
// at the next observation point. A queue whose kernel sequence the stream has
// submitted before is replayed through a graph recorded for that sequence,
// with the grids, blocks and arguments that changed patched in; the first
// sight of a sequence goes launch by launch, the second records it.

// ---- resolution -------------------------------------------------------------

#define CAJ_DEFER_MAX_ARGS 48
#define CAJ_DEFER_BLOB     384

struct caj_kres {
    const char* key;                  // the launch site's name pointer
    char name[128];                   // ... and what it pointed at, verified per hit
    unsigned gen;                     // g_xpu_registry_gen this was resolved at
    struct cajeta_xpu_module* mod;    // the CUDA module entry, or NULL
    struct cajeta_kparams* kp;
    struct cajeta_optix_rq* rq;
    uint32_t dynShared;               // relocated `extern .shared` bytes, 0 for most
    uint8_t hasAccel;                 // a param is an AccelerationStructure
    uint8_t needsXlat;                // a param is a texture, image or buffer array
    uint8_t queueable;                // every param can be copied by value
    uint8_t alignChecked;             // a buffer param's base must be aligned
    uint16_t nargs;
    uint16_t argBytes[CAJ_DEFER_MAX_ARGS];
    uint16_t argOff[CAJ_DEFER_MAX_ARGS];
    uint16_t blobLen;
};
#define CAJ_KRES_SLOTS 2048           // a power of two
#define CAJ_KRES_PROBE 8
static struct caj_kres g_kres[CAJ_KRES_SLOTS];
static pthread_mutex_t g_kres_lock = PTHREAD_MUTEX_INITIALIZER;

static void caj_kres_fill(struct caj_kres* r, const char* name) {
    r->kp = cajeta_xpu_find_kparams(name);
    r->rq = cajeta_xpu_find_optix_rq(name);
    r->dynShared = cajeta_xpu_dynamic_shared_bytes(name);
    pthread_mutex_lock(&g_xpu_cuda_lock);
    r->mod = cajeta_xpu_find_module(name, CAJ_XPU_CUDA);
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    r->hasAccel = 0; r->needsXlat = 0; r->nargs = 0; r->blobLen = 0;
    r->alignChecked = (r->kp && r->kp->align) ? 1 : 0;
    // No param record means the argument sizes are unknown (a kernel with no
    // parameters registers none either, and is rare enough to go directly).
    r->queueable = (r->kp && r->kp->count > 0) ? 1 : 0;
    if (r->kp && r->kp->count > 0) {
        unsigned off = 0;
        if (r->kp->count > CAJ_DEFER_MAX_ARGS) r->queueable = 0;
        for (int i = 0; i < r->kp->count; ++i) {
            uint8_t k = r->kp->kind[i];
            unsigned bytes = 0;
            if (k == CAJETA_KP_BUFFER) bytes = sizeof(cajeta_cudeviceptr);
            else if (k == CAJETA_KP_SCALAR) bytes = r->kp->byteSize[i];
            if (k == CAJETA_KP_ACCEL) r->hasAccel = 1;
            if (k == CAJETA_KP_TEXTURE || k == CAJETA_KP_IMAGE ||
                k == CAJETA_KP_BUFFER_ARRAY) r->needsXlat = 1;
            if (bytes == 0) r->queueable = 0;
            if (i < CAJ_DEFER_MAX_ARGS) {
                r->argBytes[i] = (uint16_t) bytes;
                r->argOff[i] = (uint16_t) off;
            }
            off += (bytes + 7u) & ~7u;
            if (off > CAJ_DEFER_BLOB) r->queueable = 0;
        }
        if (r->queueable) { r->nargs = (uint16_t) r->kp->count; r->blobLen = (uint16_t) off; }
    }
}

// The resolved record for a launch-site name. `scratch` serves a name too long
// to cache; the record is valid until the next registration.
static struct caj_kres* caj_kres_get(const char* name, struct caj_kres* scratch) {
    size_t len = strlen(name);
    if (len >= sizeof(((struct caj_kres*) 0)->name)) {
        memset(scratch, 0, sizeof(*scratch));
        caj_kres_fill(scratch, name);
        return scratch;
    }
    uintptr_t h = (uintptr_t) name;
    h ^= h >> 17; h *= (uintptr_t) 0x9E3779B97F4A7C15ull; h ^= h >> 29;
    unsigned base = (unsigned) h & (CAJ_KRES_SLOTS - 1);
    unsigned gen = g_xpu_registry_gen;
    struct caj_kres* r = NULL;
    pthread_mutex_lock(&g_kres_lock);
    for (int p = 0; p < CAJ_KRES_PROBE; ++p) {
        struct caj_kres* c = &g_kres[(base + (unsigned) p) & (CAJ_KRES_SLOTS - 1)];
        if (c->key == name) { r = c; break; }
        if (!c->key && !r) r = c;
    }
    if (!r) r = &g_kres[base];                    // every probe taken: replace the first
    // A JIT that died can hand its name's address to another string, so the
    // pointer is the hash and the bytes are the identity.
    if (r->key != name || r->gen != gen || memcmp(r->name, name, len + 1) != 0) {
        r->key = name;
        memcpy(r->name, name, len + 1);
        r->gen = gen;
        caj_kres_fill(r, name);
    }
    pthread_mutex_unlock(&g_kres_lock);
    return r;
}

// ---- deferral ---------------------------------------------------------------

// CUDA_KERNEL_NODE_PARAMS_v2, spelled here because no CUDA header is included.
struct caj_cu_kernel_node {
    void* func;
    unsigned gridX, gridY, gridZ, blockX, blockY, blockZ, sharedBytes;
    void** kernelParams;
    void** extra;
    void* kern;
    void* ctx;
};

struct caj_dl {                       // one queued launch
    void* fn;
    const char* name;
    unsigned grid[3], block[3], shared;
    uint16_t nargs, blobLen;
    uint16_t off[CAJ_DEFER_MAX_ARGS];
    uint8_t blob[CAJ_DEFER_BLOB];
};

struct caj_dgraph {                   // one recorded sequence
    uint64_t sig;
    int n;
    void* graph;
    void* exec;
    void** nodes;
    struct caj_dl* last;              // what the executable graph currently holds
    uint64_t use;
};

#define CAJ_DEFER_STREAMS 8
#define CAJ_DEFER_GRAPHS  32
#define CAJ_DEFER_CANDS   32
#define CAJ_DEFER_QUEUE_MAX 8192      // a queue this long submits itself
#define CAJ_DEFER_MIN_REPLAY 3        // a graph costs one launch; below this it cannot pay

struct caj_dstream {
    int used, enabled;
    void* stream;
    struct caj_dl* q;
    int n, cap;
    struct caj_dgraph g[CAJ_DEFER_GRAPHS];
    uint64_t cand[CAJ_DEFER_CANDS];
    int candNext;
    uint64_t tick;
};
static struct caj_dstream g_dstreams[CAJ_DEFER_STREAMS];
static pthread_mutex_t g_defer_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_defer_stats[8];      // KernelStream.DEFER_*
enum { CAJ_DS_QUEUED = 0, CAJ_DS_REPLAYED, CAJ_DS_DIRECT, CAJ_DS_GRAPHS,
       CAJ_DS_GRAPH_LAUNCHES, CAJ_DS_PATCHES, CAJ_DS_FLUSHES };

// CAJETA_XPU_DEFER: "0" never queues (the A/B control), "all" defers every
// stream from its first launch (the whole-suite validation lever), anything
// else or unset leaves the choice to KernelStream.setDeferred.
static int caj_defer_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char* e = getenv("CAJETA_XPU_DEFER");
        if (e && e[0] == '0') mode = 0;
        else if (e && strcmp(e, "all") == 0) mode = 2;
        else mode = 1;
    }
    return mode;
}

static struct caj_dstream* caj_dstream_find(void* stream, int create) {
    struct caj_dstream* freeSlot = NULL;
    for (int i = 0; i < CAJ_DEFER_STREAMS; ++i) {
        if (g_dstreams[i].used && g_dstreams[i].stream == stream) return &g_dstreams[i];
        if (!g_dstreams[i].used && !freeSlot) freeSlot = &g_dstreams[i];
    }
    if (!create || !freeSlot) return NULL;
    memset(freeSlot, 0, sizeof(*freeSlot));
    freeSlot->used = 1;
    freeSlot->stream = stream;
    return freeSlot;
}

static void caj_dgraph_drop(struct caj_dgraph* g) {
    if (g->exec && g_xpu_cuda_real.cuGraphExecDestroy) g_xpu_cuda_real.cuGraphExecDestroy(g->exec);
    if (g->graph && g_xpu_cuda_real.cuGraphDestroy) g_xpu_cuda_real.cuGraphDestroy(g->graph);
    free(g->nodes);
    free(g->last);
    memset(g, 0, sizeof(*g));
}

static int caj_defer_graphs_bound(void) {
    return g_xpu_cuda_real.cuGraphCreate && g_xpu_cuda_real.cuGraphAddKernelNode
        && g_xpu_cuda_real.cuGraphInstantiate && g_xpu_cuda_real.cuGraphLaunch
        && g_xpu_cuda_real.cuGraphExecKernelNodeSetParams
        && g_xpu_cuda_real.cuGraphExecDestroy && g_xpu_cuda_real.cuGraphDestroy;
}

static void caj_dl_node(const struct caj_dl* l, void** argv, struct caj_cu_kernel_node* kn) {
    for (int a = 0; a < l->nargs; ++a) argv[a] = (void*) (l->blob + l->off[a]);
    memset(kn, 0, sizeof(*kn));
    kn->func = l->fn;
    kn->gridX = l->grid[0]; kn->gridY = l->grid[1]; kn->gridZ = l->grid[2];
    kn->blockX = l->block[0]; kn->blockY = l->block[1]; kn->blockZ = l->block[2];
    kn->sharedBytes = l->shared;
    kn->kernelParams = argv;
}

static int caj_dl_same(const struct caj_dl* a, const struct caj_dl* b) {
    return memcmp(a->grid, b->grid, sizeof(a->grid)) == 0
        && memcmp(a->block, b->block, sizeof(a->block)) == 0
        && a->shared == b->shared && a->blobLen == b->blobLen
        && memcmp(a->blob, b->blob, a->blobLen) == 0;
}

// Records the queue as a graph: one kernel node per launch, each depending on
// the one before it, which is the order a stream gives them.
static struct caj_dgraph* caj_dgraph_build(struct caj_dstream* ds, uint64_t sig) {
    struct caj_dgraph* g = &ds->g[0];
    for (int i = 0; i < CAJ_DEFER_GRAPHS; ++i) {
        if (!ds->g[i].exec) { g = &ds->g[i]; break; }
        if (ds->g[i].use < g->use) g = &ds->g[i];
    }
    if (g->exec) caj_dgraph_drop(g);
    const int n = ds->n;
    g->nodes = (void**) calloc((size_t) n, sizeof(void*));
    g->last = (struct caj_dl*) malloc((size_t) n * sizeof(struct caj_dl));
    if (!g->nodes || !g->last || g_xpu_cuda_real.cuGraphCreate(&g->graph, 0) != 0) {
        caj_dgraph_drop(g);
        return NULL;
    }
    for (int i = 0; i < n; ++i) {
        void* argv[CAJ_DEFER_MAX_ARGS];
        struct caj_cu_kernel_node kn;
        caj_dl_node(&ds->q[i], argv, &kn);
        const void* dep = i ? g->nodes[i - 1] : NULL;
        if (g_xpu_cuda_real.cuGraphAddKernelNode(&g->nodes[i], g->graph,
                                                 i ? &dep : NULL, i ? 1 : 0, &kn) != 0) {
            caj_dgraph_drop(g);
            return NULL;
        }
    }
    if (g_xpu_cuda_real.cuGraphInstantiate(&g->exec, g->graph, 0) != 0) {
        g->exec = NULL;
        caj_dgraph_drop(g);
        return NULL;
    }
    memcpy(g->last, ds->q, (size_t) n * sizeof(struct caj_dl));
    g->sig = sig;
    g->n = n;
    g_defer_stats[CAJ_DS_GRAPHS]++;
    return g;
}

// Submits one stream's queue. Caller holds g_defer_lock.
static void caj_defer_flush_locked(struct caj_dstream* ds) {
    const int n = ds->n;
    if (n == 0) return;
    const int64_t profT0 = caj_drv_prof() ? caj_drv_prof_now() : 0;
    if (g_xpu_cuda_real.cuCtxSetCurrent) g_xpu_cuda_real.cuCtxSetCurrent(g_xpu_cuda.ctx);
    g_defer_stats[CAJ_DS_FLUSHES]++;
    int done = 0;
    if (n >= CAJ_DEFER_MIN_REPLAY && caj_defer_graphs_bound()) {
        uint64_t sig = 1469598103934665603ull ^ (uint64_t) n;
        for (int i = 0; i < n; ++i) {
            sig ^= (uint64_t) (uintptr_t) ds->q[i].fn + (uint64_t) ds->q[i].nargs;
            sig *= 1099511628211ull;
        }
        struct caj_dgraph* g = NULL;
        for (int i = 0; i < CAJ_DEFER_GRAPHS && !g; ++i) {
            struct caj_dgraph* c = &ds->g[i];
            if (!c->exec || c->sig != sig || c->n != n) continue;
            int same = 1;
            for (int k = 0; k < n && same; ++k)
                same = c->last[k].fn == ds->q[k].fn && c->last[k].nargs == ds->q[k].nargs;
            if (same) g = c;
        }
        int fresh = 0;
        if (!g) {
            int seen = 0;
            for (int i = 0; i < CAJ_DEFER_CANDS; ++i) if (ds->cand[i] == sig) seen = 1;
            if (seen) { g = caj_dgraph_build(ds, sig); fresh = g != NULL; }
            else { ds->cand[ds->candNext] = sig; ds->candNext = (ds->candNext + 1) % CAJ_DEFER_CANDS; }
        }
        if (g) {
            int ok = 1;
            if (!fresh) {
                for (int i = 0; i < n && ok; ++i) {
                    if (caj_dl_same(&g->last[i], &ds->q[i])) continue;
                    void* argv[CAJ_DEFER_MAX_ARGS];
                    struct caj_cu_kernel_node kn;
                    caj_dl_node(&ds->q[i], argv, &kn);
                    if (g_xpu_cuda_real.cuGraphExecKernelNodeSetParams(g->exec, g->nodes[i], &kn) != 0) {
                        ok = 0;
                    } else {
                        g->last[i] = ds->q[i];
                        g_defer_stats[CAJ_DS_PATCHES]++;
                    }
                }
            }
            // Nothing has been submitted yet, so a recording the driver refuses
            // to patch or launch is dropped and the queue goes one by one.
            if (ok && g_xpu_cuda_real.cuGraphLaunch(g->exec, ds->stream) == 0) {
                g->use = ++ds->tick;
                g_defer_stats[CAJ_DS_GRAPH_LAUNCHES]++;
                g_defer_stats[CAJ_DS_REPLAYED] += n;
                done = 1;
            } else {
                caj_dgraph_drop(g);
            }
        }
    }
    if (!done) {
        for (int i = 0; i < n; ++i) {
            struct caj_dl* l = &ds->q[i];
            void* argv[CAJ_DEFER_MAX_ARGS];
            for (int a = 0; a < l->nargs; ++a) argv[a] = (void*) (l->blob + l->off[a]);
            int rc = g_xpu_cuda_real.cuLaunchKernel(l->fn, l->grid[0], l->grid[1], l->grid[2],
                                                    l->block[0], l->block[1], l->block[2],
                                                    l->shared, ds->stream, argv, NULL);
            if (rc != 0) {
                cajeta_xpu_note_launch_failure();
                fprintf(stderr, "cajeta.xpu: cuLaunchKernel('%s') failed (%d) "
                        "submitting a deferred launch\n", l->name ? l->name : "?", rc);
            }
        }
        g_defer_stats[CAJ_DS_DIRECT] += n;
    }
    __atomic_fetch_sub(&g_defer_pending, n, __ATOMIC_RELAXED);
    ds->n = 0;
    if (profT0) caj_drv_prof_add(CAJ_DP_deferSubmit, profT0);
}

static void caj_defer_flush_all(void) {
    pthread_mutex_lock(&g_defer_lock);
    for (int i = 0; i < CAJ_DEFER_STREAMS; ++i)
        if (g_dstreams[i].used && g_dstreams[i].n) caj_defer_flush_locked(&g_dstreams[i]);
    pthread_mutex_unlock(&g_defer_lock);
}

static void caj_defer_flush_stream(void* stream) {
    pthread_mutex_lock(&g_defer_lock);
    struct caj_dstream* ds = caj_dstream_find(stream, 0);
    if (ds && ds->n) caj_defer_flush_locked(ds);
    pthread_mutex_unlock(&g_defer_lock);
}

// The stream is being destroyed: its queue was just submitted; drop its recordings.
static void caj_defer_stream_gone(void* stream) {
    pthread_mutex_lock(&g_defer_lock);
    struct caj_dstream* ds = caj_dstream_find(stream, 0);
    if (ds) {
        for (int i = 0; i < CAJ_DEFER_GRAPHS; ++i)
            if (ds->g[i].exec || ds->g[i].graph) caj_dgraph_drop(&ds->g[i]);
        free(ds->q);
        memset(ds, 0, sizeof(*ds));
    }
    pthread_mutex_unlock(&g_defer_lock);
}

// Is this stream queueing? The profiler attributes each launch at the moment it
// is submitted, so an armed profiler keeps every launch direct.
static int caj_defer_wants(void* stream) {
    int mode = caj_defer_mode();
    if (mode == 0) return 0;
    if (__cajeta_prof_gpu_sink_count() != 0) return 0;
    if (mode == 2) return 1;
    // Unlocked read of a flag a launching thread set itself; a stream nobody
    // deferred has no record and costs eight compares.
    for (int i = 0; i < CAJ_DEFER_STREAMS; ++i)
        if (g_dstreams[i].used && g_dstreams[i].stream == stream) return g_dstreams[i].enabled;
    return 0;
}

// Queues one launch; 0 when it could not be queued and must go directly.
static int caj_defer_enqueue(void* stream, const struct caj_kres* r, void* fn,
                             const char* name, const unsigned grid[3],
                             const unsigned block[3], unsigned shared, void** argv) {
    pthread_mutex_lock(&g_defer_lock);
    struct caj_dstream* ds = caj_dstream_find(stream, 1);
    if (!ds) { pthread_mutex_unlock(&g_defer_lock); return 0; }
    if (ds->n == ds->cap) {
        if (ds->cap >= CAJ_DEFER_QUEUE_MAX) {
            caj_defer_flush_locked(ds);
        } else {
            int cap = ds->cap ? ds->cap * 2 : 64;
            struct caj_dl* q = (struct caj_dl*) realloc(ds->q, (size_t) cap * sizeof(struct caj_dl));
            if (!q) { pthread_mutex_unlock(&g_defer_lock); return 0; }
            ds->q = q;
            ds->cap = cap;
        }
    }
    struct caj_dl* l = &ds->q[ds->n];
    l->fn = fn;
    l->name = name;
    memcpy(l->grid, grid, sizeof(l->grid));
    memcpy(l->block, block, sizeof(l->block));
    l->shared = shared;
    l->nargs = r->nargs;
    l->blobLen = r->blobLen;
    // Zeroed first: the padding between arguments is compared when a
    // recording is patched.
    memset(l->blob, 0, r->blobLen);
    for (int a = 0; a < r->nargs; ++a) {
        l->off[a] = r->argOff[a];
        memcpy(l->blob + r->argOff[a], argv[a], r->argBytes[a]);
    }
    ds->n++;
    __atomic_fetch_add(&g_defer_pending, 1, __ATOMIC_RELAXED);
    g_defer_stats[CAJ_DS_QUEUED]++;
    pthread_mutex_unlock(&g_defer_lock);
    return 1;
}

// ---- KernelStream natives ---------------------------------------------------

void __cajeta_xpu_stream_set_deferred(int64_t handle, int32_t on) {
    if (cajeta_xpu_active_backend() != CAJ_XPU_CUDA) return;   // nothing to defer
    void* stream = (void*) (intptr_t) handle;
    pthread_mutex_lock(&g_defer_lock);
    struct caj_dstream* ds = caj_dstream_find(stream, on ? 1 : 0);
    if (ds) {
        if (!on && ds->n) caj_defer_flush_locked(ds);
        ds->enabled = on ? 1 : 0;
    }
    pthread_mutex_unlock(&g_defer_lock);
}

void __cajeta_xpu_stream_flush(int64_t handle) {
    if (!__atomic_load_n(&g_defer_pending, __ATOMIC_RELAXED)) return;
    caj_defer_flush_stream((void*) (intptr_t) handle);
}

int64_t __cajeta_xpu_stream_pending(int64_t handle) {
    int64_t n = 0;
    pthread_mutex_lock(&g_defer_lock);
    struct caj_dstream* ds = caj_dstream_find((void*) (intptr_t) handle, 0);
    if (ds) n = ds->n;
    pthread_mutex_unlock(&g_defer_lock);
    return n;
}

int64_t __cajeta_xpu_defer_stat(int32_t which) {
    if (which < 0 || which > CAJ_DS_FLUSHES) return 0;
    pthread_mutex_lock(&g_defer_lock);
    int64_t v = g_defer_stats[which];
    pthread_mutex_unlock(&g_defer_lock);
    return v;
}
