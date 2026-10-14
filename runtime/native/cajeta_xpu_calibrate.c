/* cajeta_xpu_calibrate.c -- the replay half of `cajeta calibrate` (xpu-tile-manifest §14.7,
 * xpu-tile-scheduling §12.4c). Included from cajeta_xpu.c after cajeta_xpu_record.c. */

#define CAJ_CALIB_REPS 5
#define CAJ_CALIB_MAX_ARGS 64
#define CAJ_CALIB_MAX_KERNELS 512

struct caj_calib_kernel {
    char name[256];
    char* payload;
    size_t len, cap;
};

static struct caj_calib_kernel g_calib_kernels[CAJ_CALIB_MAX_KERNELS];
static int g_calib_kernel_count;

/* The key text CalibrationKey.text() gives for `subject` on this device. */
static size_t caj_calib_key_text(const char* subject, char* out, size_t cap) {
    char arch[128], power[256], uuid[33];
    caj_xpu_calib_arch(arch, sizeof arch);
    caj_xpu_calib_power(power, sizeof power);
    uuid[0] = 0;
    int backend = cajeta_xpu_active_backend();
    if (backend != CAJ_XPU_CPU) {
        if (g_xpu_geo_state == 0) {
            g_xpu_geo_state = cajeta_xpu_query_raw_device(&g_xpu_geo) && g_xpu_geo.valid ? 1 : -1;
        }
        if (g_xpu_geo_state == 1 && g_xpu_geo.hasUuid) {
            for (int i = 0; i < 16; i++) snprintf(uuid + 2 * i, 3, "%02x", g_xpu_geo.uuid[i]);
        }
    }
    int n = snprintf(out, cap, "%s\n%s/%s/%s\n%d\n%s\n%s", subject, caj_record_backend_name(backend),
                     arch, uuid, (int) __cajeta_xpu_driver_version(), CAJETA_RT_VERSION, power);
    return n < 0 ? 0 : (size_t) n;
}

/* A CalibrationRecord: magic, key, payload length, payload, XXH3 (seed 0) of all of that in hex. */
static char* caj_calib_encode(const char* key, const char* payload, size_t plen, size_t* outLen) {
    char head[4096];
    int h = snprintf(head, sizeof head, "cajeta-calibration-1\n%s\n%zu\n", key, plen);
    if (h <= 0 || (size_t) h >= sizeof head) return NULL;
    size_t total = (size_t) h + plen + 16;
    char* buf = (char*) malloc(total + 1);
    if (!buf) return NULL;
    memcpy(buf, head, (size_t) h);
    memcpy(buf + h, payload, plen);
    unsigned long long sum = (unsigned long long) XXH3_64bits_withSeed(buf, (size_t) h + plen, 0);
    snprintf(buf + h + plen, 17, "%016llx", sum);
    *outLen = total;
    return buf;
}

static void caj_calib_append(const char* name, const char* line) {
    struct caj_calib_kernel* k = NULL;
    for (int i = 0; i < g_calib_kernel_count; i++)
        if (strcmp(g_calib_kernels[i].name, name) == 0) k = &g_calib_kernels[i];
    if (!k) {
        if (g_calib_kernel_count >= CAJ_CALIB_MAX_KERNELS) return;
        k = &g_calib_kernels[g_calib_kernel_count++];
        snprintf(k->name, sizeof k->name, "%s", name);
    }
    size_t n = strlen(line);
    if (k->len + n + 1 > k->cap) {
        size_t cap = (k->cap ? k->cap * 2 : 256) + n;
        char* p = (char*) realloc(k->payload, cap);
        if (!p) return;
        k->payload = p;
        k->cap = cap;
    }
    memcpy(k->payload + k->len, line, n);
    k->len += n;
    k->payload[k->len] = 0;
}

static char* caj_calib_slurp(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char* buf = (char*) malloc((size_t) n + 9);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf + 8, 1, (size_t) n, f);
    fclose(f);
    buf[8 + got] = 0;
    *len = got;
    return buf;
}

/* The integers after `"key":[` (or `"key":` for one), up to `max`; returns how many were read. */
static int caj_calib_ints(const char* json, const char* key, long long* out, int max) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char* p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    int n = 0;
    if (*p == '[') {
        p++;
        while (*p && *p != ']' && n < max) {
            out[n++] = strtoll(p, (char**) &p, 10);
            while (*p == ',' || *p == ' ') p++;
        }
        return n;
    }
    out[0] = strtoll(p, NULL, 10);
    return 1;
}

/* Replays one recorded launch solo; returns the median of CAJ_CALIB_REPS timed runs in ns, or -1. */
static int64_t caj_calib_replay_one(const char* dir, const char* name, char* line, size_t lineCap) {
    char path[4352];
    snprintf(path, sizeof path, "%s/launch.json", dir);
    size_t jl = 0;
    char* raw = caj_calib_slurp(path, &jl);
    if (!raw) return -1;
    const char* json = raw + 8;
    long long grid[3] = {1, 1, 1}, block[3] = {1, 1, 1}, shared = 0;
    caj_calib_ints(json, "grid", grid, 3);
    caj_calib_ints(json, "block", block, 3);
    caj_calib_ints(json, "sharedBytes", &shared, 1);
    long long allocBytes[64];
    int nAlloc = 0;
    const char* ap = strstr(json, "\"allocs\":[");
    const char* argp = strstr(json, "\"args\":[");
    if (!ap || !argp) { free(raw); return -1; }
    for (const char* q = ap; q < argp && nAlloc < 64; ) {
        q = strstr(q, "\"bytes\":");
        if (!q || q > argp) break;
        allocBytes[nAlloc++] = strtoll(q + 8, NULL, 10);
        q += 8;
    }
    int64_t handles[64];
    for (int k = 0; k < nAlloc; k++) handles[k] = 0;
    void* av[CAJ_CALIB_MAX_ARGS];
    int64_t bufval[CAJ_CALIB_MAX_ARGS];
    unsigned char scal[CAJ_CALIB_MAX_ARGS][16];
    int nArgs = 0, ok = 1;
    for (int k = 0; k < nAlloc && ok; k++) {
        handles[k] = __cajeta_xpu_buffer_alloc(NULL, (uint64_t) allocBytes[k], CAJ_MEMKIND_DEVICE);
        if (!handles[k]) { ok = 0; break; }
        snprintf(path, sizeof path, "%s/a%d.in", dir, k);
        size_t il = 0;
        char* in = caj_calib_slurp(path, &il);
        if (in && il == (size_t) allocBytes[k]) __cajeta_xpu_buffer_upload(NULL, handles[k], in, il);
        free(in);
    }
    const char* p = argp + 8;
    while (ok && *p && *p != ']' && nArgs < CAJ_CALIB_MAX_ARGS) {
        const char* obj = strchr(p, '{');
        if (!obj) break;
        const char* end = strchr(obj, '}');
        if (!end) { ok = 0; break; }
        if (strncmp(obj, "{\"kind\":\"buffer\"", 16) == 0) {
            const char* a = strstr(obj, "\"alloc\":");
            const char* o = strstr(obj, "\"offset\":");
            int alloc = a && a < end ? atoi(a + 8) : -1;
            long long off = o && o < end ? strtoll(o + 9, NULL, 10) : 0;
            if (alloc < 0 || alloc >= nAlloc) { ok = 0; break; }
            bufval[nArgs] = handles[alloc] + off;
            av[nArgs] = &bufval[nArgs];
        } else if (strncmp(obj, "{\"kind\":\"scalar\"", 16) == 0) {
            const char* h = strstr(obj, "\"hex\":\"");
            if (!h || h > end) { ok = 0; break; }
            h += 7;
            int b = 0;
            while (h[0] && h[0] != '"' && h[1] && b < 16) {
                char two[3] = {h[0], h[1], 0};
                scal[nArgs][b++] = (unsigned char) strtoul(two, NULL, 16);
                h += 2;
            }
            av[nArgs] = scal[nArgs];
        } else {
            ok = 0;
            break;
        }
        nArgs++;
        p = end + 1;
    }
    int64_t median = -1;
    if (ok) {
        int64_t before = __cajeta_xpu_launch_failures();
        __cajeta_xpu_launch_v3(name, (int32_t) grid[0], (int32_t) grid[1], (int32_t) grid[2],
                               (int32_t) block[0], (int32_t) block[1], (int32_t) block[2],
                               (uint32_t) shared, av, 0, -1, 0, NULL);
        cajeta_xpu_sync_active();
        int64_t t[CAJ_CALIB_REPS];
        for (int r = 0; r < CAJ_CALIB_REPS; r++) {
            int64_t t0 = caj_xpu_now_ns();
            __cajeta_xpu_launch_v3(name, (int32_t) grid[0], (int32_t) grid[1], (int32_t) grid[2],
                                   (int32_t) block[0], (int32_t) block[1], (int32_t) block[2],
                                   (uint32_t) shared, av, 0, -1, 0, NULL);
            cajeta_xpu_sync_active();
            t[r] = caj_xpu_now_ns() - t0;
        }
        if (__cajeta_xpu_launch_failures() == before) {
            for (int i = 1; i < CAJ_CALIB_REPS; i++)
                for (int j = i; j > 0 && t[j - 1] > t[j]; j--) { int64_t x = t[j]; t[j] = t[j - 1]; t[j - 1] = x; }
            median = t[CAJ_CALIB_REPS / 2];
            snprintf(line, lineCap, "launch grid=%lldx%lldx%lld block=%lldx%lldx%lld shared=%lld ns=%lld\n",
                     grid[0], grid[1], grid[2], block[0], block[1], block[2], shared, (long long) median);
        }
    }
    for (int k = 0; k < nAlloc; k++)
        if (handles[k]) __cajeta_xpu_buffer_free(NULL, handles[k], CAJ_MEMKIND_DEVICE);
    free(raw);
    return median;
}

/* The kernel's code hash from its manifest ("sha256:..."), or 0 when it has none. */
static int caj_calib_code_hash(const char* name, char* out, size_t cap) {
    uint64_t len = 0;
    const char* json = caj_xpu_manifest_text(name, &len);
    if (!json) return 0;
    const char* key = "\"codeHash\":";
    size_t kl = strlen(key);
    for (uint64_t i = 0; i + kl < len; i++) {
        if (memcmp(json + i, key, kl) != 0) continue;
        const char* s = json + i + kl;
        while (s < json + len && (*s == ' ' || *s == '\t')) s++;
        if (s >= json + len || *s != '"') return 0;
        s++;
        size_t n = 0;
        while (s + n < json + len && s[n] != '"' && n + 1 < cap) n++;
        memcpy(out, s, n);
        out[n] = 0;
        return n > 0;
    }
    return 0;
}

/* Replays every launch indexed under `root` solo and writes one record per kernel to `outPath`
 * as a calibration snapshot; returns the number of kernels written, or -1 on an unreadable index. */
int32_t __cajeta_xpu_calibrate_replay(const char* root, const char* outPath) {
    char path[4352];
    snprintf(path, sizeof path, "%s/index.tsv", root);
    FILE* idx = fopen(path, "r");
    if (!idx) return -1;
    char row[4608];
    int replayed = 0, skipped = 0;
    while (fgets(row, sizeof row, idx)) {
        char* tab = strchr(row, '\t');
        if (!tab) continue;
        *tab = 0;
        char* name = tab + 1;
        name[strcspn(name, "\r\n")] = 0;
        char line[256];
        if (caj_calib_replay_one(row, name, line, sizeof line) >= 0) {
            caj_calib_append(name, line);
            replayed++;
        } else {
            skipped++;
        }
    }
    fclose(idx);
    FILE* out = fopen(outPath, "wb");
    if (!out) return -1;
    fprintf(out, "cajeta-calibration-snapshot-1\n");
    int written = 0;
    for (int i = 0; i < g_calib_kernel_count; i++) {
        struct caj_calib_kernel* k = &g_calib_kernels[i];
        char hash[160], key[1024];
        if (!caj_calib_code_hash(k->name, hash, sizeof hash)) continue;
        caj_calib_key_text(hash, key, sizeof key);
        size_t rl = 0;
        char* rec = caj_calib_encode(key, k->payload, k->len, &rl);
        if (!rec) continue;
        unsigned long long slot = (unsigned long long) XXH3_64bits_withSeed(key, strlen(key), 0);
        fprintf(out, "cal-%016llx %zu %s\n", slot, rl, k->name);
        fwrite(rec, 1, rl, out);
        fputc('\n', out);
        free(rec);
        written++;
    }
    fclose(out);
    fprintf(stderr, "cajeta calibrate: %d launch(es) replayed, %d skipped, %d kernel record(s) written to %s\n",
            replayed, skipped, written, outPath);
    return written;
}

/* `cajeta calibrate`'s replay pass, called from the generated main: with CAJETA_XPU_CALIBRATE_REPLAY
 * set it replays that recording into CAJETA_XPU_CALIBRATE_OUT and exits; otherwise it returns 0. */
int32_t __cajeta_xpu_calib_arm(void) {
    const char* dir = getenv("CAJETA_XPU_CALIBRATE_REPLAY");
    if (!dir || !*dir) return 0;
    const char* out = getenv("CAJETA_XPU_CALIBRATE_OUT");
    if (!out || !*out) {
        fprintf(stderr, "cajeta calibrate: CAJETA_XPU_CALIBRATE_REPLAY is set without CAJETA_XPU_CALIBRATE_OUT\n");
        exit(2);
    }
    int32_t n = __cajeta_xpu_calibrate_replay(dir, out);
    exit(n >= 0 ? 0 : 2);
}
