// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
// CAJETA_XPU_RECORD=<dir>: record kernel launches for the conformance corpus
// (xpu-kernel-independence Unit 1). Each recorded launch is a directory
//
//   <dir>/<kernel>.<n>/launch.json   the kernel, backend, grid, block, wave,
//                                    and each argument: a buffer as an
//                                    allocation and a byte offset into it, a
//                                    scalar as its bytes in hex
//   <dir>/<kernel>.<n>/a<i>.in       allocation i before the launch
//   <dir>/<kernel>.<n>/a<i>.out      allocation i after it
//
// An allocation is recorded from the lowest byte any argument points at to
// its end, once however many arguments point into it, so two slices of one
// buffer stay one memory when the launch is replayed.
//
// The first CAJETA_XPU_RECORD_PER_KERNEL recordable launches of each kernel
// are recorded (default 2). A launch is not recorded when an argument's
// allocation is unknown, when the allocations come to more than
// CAJETA_XPU_RECORD_MAX_BYTES (default 64 MiB), or when its stream is
// deferred, since a deferred launch does not run until a later flush; each
// such launch is counted in <dir>/skipped.tsv with its reason and does not
// spend the kernel's quota. Recording synchronizes the device around
// every recorded launch, so it is for test runs, never for timing.

#include <ctype.h>
#ifdef _WIN32
#include <direct.h>
#endif

struct caj_rec_alloc { int64_t base; uint64_t bytes; };
static struct caj_rec_alloc* g_rec_allocs;
static int g_rec_alloc_count, g_rec_alloc_cap;
static pthread_mutex_t g_rec_lock = PTHREAD_MUTEX_INITIALIZER;

struct caj_rec_kernel { char name[256]; int seen; };
static struct caj_rec_kernel* g_rec_kernels;
static int g_rec_kernel_count, g_rec_kernel_cap;

// Read once per runtime instance: a launch is a hot path (hundreds per decoded
// token), so it never asks the environment again. Set the variable before the
// program's first buffer or launch.
static const char* caj_record_dir(void) {
    static int read = 0;
    static char dir[2048];
    if (!read) {
        const char* d = getenv("CAJETA_XPU_RECORD");
        if (d && *d) {
            strncpy(dir, d, sizeof dir - 1);
            dir[sizeof dir - 1] = '\0';
        }
        read = 1;
    }
    return dir[0] ? dir : NULL;
}

static void caj_record_alloc(int64_t h, uint64_t bytes) {
    if (!h || !caj_record_dir()) return;
    pthread_mutex_lock(&g_rec_lock);
    if (g_rec_alloc_count == g_rec_alloc_cap) {
        int cap = g_rec_alloc_cap ? g_rec_alloc_cap * 2 : 1024;
        struct caj_rec_alloc* grown = (struct caj_rec_alloc*)
            realloc(g_rec_allocs, (size_t) cap * sizeof(*grown));
        if (!grown) { pthread_mutex_unlock(&g_rec_lock); return; }
        g_rec_allocs = grown;
        g_rec_alloc_cap = cap;
    }
    g_rec_allocs[g_rec_alloc_count].base = h;
    g_rec_allocs[g_rec_alloc_count].bytes = bytes;
    g_rec_alloc_count++;
    pthread_mutex_unlock(&g_rec_lock);
}

static void caj_record_free(int64_t h) {
    if (!h || !g_rec_alloc_count) return;
    pthread_mutex_lock(&g_rec_lock);
    for (int i = 0; i < g_rec_alloc_count; i++)
        if (g_rec_allocs[i].base == h) {
            g_rec_allocs[i] = g_rec_allocs[--g_rec_alloc_count];
            break;
        }
    pthread_mutex_unlock(&g_rec_lock);
}

// The live allocation holding byte `p`, or -1. Pointer backends only: a
// Vulkan handle is a table index, not an address.
static int caj_record_find(int64_t p) {
    for (int i = 0; i < g_rec_alloc_count; i++)
        if (p >= g_rec_allocs[i].base &&
            (uint64_t) (p - g_rec_allocs[i].base) < g_rec_allocs[i].bytes)
            return i;
    return -1;
}

// The quota entry for `name`, made on first sight; NULL when out of memory.
static struct caj_rec_kernel* caj_record_entry(const char* name) {
    for (int i = 0; i < g_rec_kernel_count; i++)
        if (strncmp(g_rec_kernels[i].name, name, sizeof(g_rec_kernels[i].name)) == 0)
            return &g_rec_kernels[i];
    if (g_rec_kernel_count == g_rec_kernel_cap) {
        int cap = g_rec_kernel_cap ? g_rec_kernel_cap * 2 : 256;
        struct caj_rec_kernel* grown = (struct caj_rec_kernel*)
            realloc(g_rec_kernels, (size_t) cap * sizeof(*grown));
        if (!grown) return NULL;
        g_rec_kernels = grown;
        g_rec_kernel_cap = cap;
    }
    struct caj_rec_kernel* k = &g_rec_kernels[g_rec_kernel_count++];
    strncpy(k->name, name, sizeof(k->name) - 1);
    k->name[sizeof(k->name) - 1] = '\0';
    k->seen = 0;
    return k;
}

// How many times `name` has been recorded so far. A skipped launch is not
// a recording: it leaves the quota for a launch that can be recorded.
static int caj_record_seen(const char* name) {
    struct caj_rec_kernel* k = caj_record_entry(name);
    return k ? k->seen : 1 << 30;
}

// Spend one of `name`'s recordings, returning its ordinal.
static int caj_record_take(const char* name) {
    struct caj_rec_kernel* k = caj_record_entry(name);
    return k ? ++k->seen : 1 << 30;
}

static uint64_t caj_record_env_u64(const char* var, uint64_t dflt) {
    const char* e = getenv(var);
    return (e && *e) ? strtoull(e, NULL, 10) : dflt;
}

static void caj_record_mkdir(const char* dir) {
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
}

// The skip log lives under the root, so the root exists before the first
// recording as well as after it.
static void caj_record_skip(const char* dir, const char* name, const char* why) {
    char path[4096];
    caj_record_mkdir(dir);
    snprintf(path, sizeof path, "%s/skipped.tsv", dir);
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s\t%s\n", name, why);
    fclose(f);
}

static const char* caj_record_backend_name(int b) {
    switch (b) {
        case CAJ_XPU_CUDA: return "cuda";
        case CAJ_XPU_HIP: return "hip";
        case CAJ_XPU_VULKAN: return "vulkan";
        case CAJ_XPU_CPU: return "cpu";
        default: return "none";
    }
}

// The wave width the backend built `name` at, from its manifest, or 0.
static int caj_record_wave(const char* name, int backend) {
    int w = 0;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    for (int i = 0; i < g_xpu_manifest_count; i++) {
        struct cajeta_xpu_manifest* e = &g_xpu_manifests[i];
        if (e->backend != backend || strncmp(e->name, name, sizeof(e->name)) != 0 || !e->json)
            continue;
        const char* key = "\"waveWidth\":";
        size_t kl = strlen(key);
        for (uint64_t j = 0; j + kl < e->len; j++)
            if (memcmp(e->json + j, key, kl) == 0) { w = atoi(e->json + j + kl); break; }
        break;
    }
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    return w;
}

static void caj_record_download(int64_t base, void* out, uint64_t bytes) {
    // __cajeta_xpu_buffer_download writes past an 8-byte array header.
    char* staging = (char*) malloc((size_t) bytes + 8);
    if (!staging) return;
    __cajeta_xpu_buffer_download(NULL, base, staging, bytes);
    memcpy(out, staging + 8, (size_t) bytes);
    free(staging);
}

static int caj_record_write(const char* path, const void* p, uint64_t bytes) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    size_t w = fwrite(p, 1, (size_t) bytes, f);
    fclose(f);
    return w == (size_t) bytes;
}

// One launch being recorded: the allocations it touches, as recorded.
struct caj_rec_launch {
    char dir[4096];
    int n;                         // allocations
    int64_t base[64];              // lowest byte an argument points at
    uint64_t bytes[64];
};

static int caj_record_begin_into(struct caj_rec_launch* L, const char* kernelName,
                                 int32_t gx, int32_t gy, int32_t gz,
                                 int32_t bx, int32_t by, int32_t bz,
                                 uint32_t sharedBytes, void* argv, int64_t stream) {
    const char* root = caj_record_dir();
    if (!root || !kernelName) return 0;
    int backend = cajeta_xpu_active_backend();
    pthread_mutex_lock(&g_rec_lock);
    int seen = caj_record_seen(kernelName);
    pthread_mutex_unlock(&g_rec_lock);
    if ((uint64_t) seen >= caj_record_env_u64("CAJETA_XPU_RECORD_PER_KERNEL", 2)) return 0;
    if (backend == CAJ_XPU_VULKAN) { caj_record_skip(root, kernelName, "vulkan handles are not addresses"); return 0; }
    if (backend == CAJ_XPU_CUDA && caj_defer_wants((void*) (intptr_t) stream)) { caj_record_skip(root, kernelName, "deferred stream"); return 0; }
    struct cajeta_kparams* kp = cajeta_xpu_find_kparams(kernelName);
    if (!kp) { caj_record_skip(root, kernelName, "no parameter metadata registered"); return 0; }
    void** av = (void**) argv;
    int argAlloc[64];
    int64_t argOff[64];
    L->n = 0;
    pthread_mutex_lock(&g_rec_lock);
    for (int i = 0; i < kp->count && i < 64; i++) {
        argAlloc[i] = -1;
        if (kp->kind[i] != CAJETA_KP_BUFFER) continue;
        int64_t p = *(int64_t*) av[i];
        int a = caj_record_find(p);
        if (a < 0) {
            pthread_mutex_unlock(&g_rec_lock);
            caj_record_skip(root, kernelName, "a buffer argument outside every known allocation");
            return 0;
        }
        // One recorded region per allocation: the region runs from the lowest
        // byte any argument points at to the allocation's end.
        const int64_t abase = g_rec_allocs[a].base;
        const int64_t aend = abase + (int64_t) g_rec_allocs[a].bytes;
        int slot = -1;
        for (int k = 0; k < L->n; k++)
            if (L->base[k] >= abase && L->base[k] < aend) slot = k;
        if (slot < 0) {
            slot = L->n++;
            L->base[slot] = p;
            L->bytes[slot] = (uint64_t) (aend - p);
        } else if (p < L->base[slot]) {
            L->bytes[slot] += (uint64_t) (L->base[slot] - p);
            L->base[slot] = p;
        }
        argAlloc[i] = slot;
        argOff[i] = p;
    }
    pthread_mutex_unlock(&g_rec_lock);
    uint64_t total = 0;
    for (int k = 0; k < L->n; k++) total += L->bytes[k];
    if (total > caj_record_env_u64("CAJETA_XPU_RECORD_MAX_BYTES", 64ull << 20)) {
        caj_record_skip(root, kernelName, "allocations larger than CAJETA_XPU_RECORD_MAX_BYTES");
        return 0;
    }

    // A file name keeps letters, digits, '_' and '.'.
    char safe[256];
    size_t j = 0;
    for (const char* c = kernelName; *c && j + 1 < sizeof safe; c++)
        safe[j++] = (isalnum((unsigned char) *c) || *c == '_' || *c == '.') ? *c : '_';
    safe[j] = 0;
    pthread_mutex_lock(&g_rec_lock);
    int nth = caj_record_take(kernelName);
    pthread_mutex_unlock(&g_rec_lock);
    snprintf(L->dir, sizeof L->dir, "%s/%s.%d", root, safe, nth);
    caj_record_mkdir(root);
    caj_record_mkdir(L->dir);
    cajeta_xpu_sync_active();
    char path[4352];
    for (int k = 0; k < L->n; k++) {
        void* host = malloc((size_t) L->bytes[k]);
        if (!host) return 0;
        caj_record_download(L->base[k], host, L->bytes[k]);
        snprintf(path, sizeof path, "%s/a%d.in", L->dir, k);
        int ok = caj_record_write(path, host, L->bytes[k]);
        free(host);
        if (!ok) return 0;
    }
    snprintf(path, sizeof path, "%s/launch.json", L->dir);
    FILE* f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "{\"kernel\":\"%s\",\"backend\":\"%s\",\"grid\":[%d,%d,%d],"
               "\"block\":[%d,%d,%d],\"sharedBytes\":%u,\"waveWidth\":%d,\"allocs\":[",
            kernelName, caj_record_backend_name(backend), gx, gy, gz, bx, by, bz,
            sharedBytes, caj_record_wave(kernelName, backend));
    for (int k = 0; k < L->n; k++)
        fprintf(f, "%s{\"bytes\":%llu}", k ? "," : "", (unsigned long long) L->bytes[k]);
    fprintf(f, "],\"args\":[");
    for (int i = 0; i < kp->count && i < 64; i++) {
        if (i) fputc(',', f);
        if (kp->kind[i] == CAJETA_KP_BUFFER) {
            fprintf(f, "{\"kind\":\"buffer\",\"alloc\":%d,\"offset\":%lld}", argAlloc[i],
                    (long long) (argOff[i] - L->base[argAlloc[i]]));
        } else if (kp->kind[i] == CAJETA_KP_SCALAR) {
            uint32_t sz = kp->byteSize[i] ? kp->byteSize[i] : 4u;
            fprintf(f, "{\"kind\":\"scalar\",\"hex\":\"");
            const unsigned char* b = (const unsigned char*) av[i];
            for (uint32_t q = 0; q < sz; q++) fprintf(f, "%02x", b[q]);
            fprintf(f, "\"}");
        } else {
            fprintf(f, "{\"kind\":\"other\",\"code\":%d}", (int) kp->kind[i]);
        }
    }
    fprintf(f, "]}\n");
    fclose(f);
    return 1;
}

// Before the launch: decide, and write the inputs. Returns the launch being
// recorded, or NULL when this one is not.
static void* caj_record_begin(const char* kernelName,
                              int32_t gx, int32_t gy, int32_t gz,
                              int32_t bx, int32_t by, int32_t bz,
                              uint32_t sharedBytes, void* argv, int64_t stream) {
    if (!caj_record_dir()) return NULL;
    struct caj_rec_launch* L = (struct caj_rec_launch*) calloc(1, sizeof *L);
    if (!L) return NULL;
    if (!caj_record_begin_into(L, kernelName, gx, gy, gz, bx, by, bz, sharedBytes,
                               argv, stream)) {
        free(L);
        return NULL;
    }
    return L;
}

// After the launch: wait for it, and write the outputs.
static void caj_record_end(void* rec) {
    struct caj_rec_launch* L = (struct caj_rec_launch*) rec;
    if (!L) return;
    cajeta_xpu_sync_active();
    char path[4352];
    for (int k = 0; k < L->n; k++) {
        void* host = malloc((size_t) L->bytes[k]);
        if (!host) return;
        caj_record_download(L->base[k], host, L->bytes[k]);
        snprintf(path, sizeof path, "%s/a%d.out", L->dir, k);
        caj_record_write(path, host, L->bytes[k]);
        free(host);
    }
    free(L);
}
