/* cajeta_xpu_calib.c -- calibration identity and embedded records (xpu-tile-scheduling §12.4).
 * Included from cajeta_xpu.c after cajeta_xpu_sched.c. */

#include "cajeta_rt_version.h"

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

static void* caj_xpu_calib_bytes(const char* s, size_t n) {
    void* hdr = __cajeta_new_array_header(8, 1, (int64_t) n);
    if (!hdr) return NULL;
    if (n) memcpy((char*) hdr + 8, s, n);
    return hdr;
}

/* The platform's user cache directory for calibration records, or NULL. */
void* __cajeta_xpu_calibration_cache_root(void) {
    char path[4096];
    path[0] = 0;
#if defined(_WIN32)
    const char* base = getenv("LOCALAPPDATA");
    if (base && base[0]) snprintf(path, sizeof path, "%s\\cajeta\\calibration", base);
#elif defined(__APPLE__)
    const char* home = getenv("HOME");
    if (home && home[0]) snprintf(path, sizeof path, "%s/Library/Caches/cajeta/calibration", home);
#else
    const char* xdg = getenv("XDG_CACHE_HOME");
    const char* home = getenv("HOME");
    if (xdg && xdg[0] == '/') snprintf(path, sizeof path, "%s/cajeta/calibration", xdg);
    else if (home && home[0]) snprintf(path, sizeof path, "%s/.cache/cajeta/calibration", home);
#endif
    if (!path[0]) return NULL;
    return caj_xpu_calib_bytes(path, strlen(path));
}

static size_t caj_xpu_calib_read_line(const char* file, char* out, size_t cap) {
    FILE* f = fopen(file, "r");
    if (!f) return 0;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) n--;
    out[n] = 0;
    return n;
}

/* The platform power profile and the GPU's performance level, "profile/level"; "" when unreadable. */
void* __cajeta_xpu_power_mode(void) {
    char mode[256];
    mode[0] = 0;
#if defined(__linux__)
    char profile[64], level[64];
    profile[0] = level[0] = 0;
    caj_xpu_calib_read_line("/sys/firmware/acpi/platform_profile", profile, sizeof profile);
    char file[96];
    for (int c = 0; c < 8 && !level[0]; c++) {
        snprintf(file, sizeof file, "/sys/class/drm/card%d/device/power_dpm_force_performance_level", c);
        caj_xpu_calib_read_line(file, level, sizeof level);
    }
    if (profile[0] || level[0]) snprintf(mode, sizeof mode, "%s/%s", profile, level);
#endif
    return caj_xpu_calib_bytes(mode, strlen(mode));
}

/* The compiler version the runtime was built with. */
void* __cajeta_xpu_compiler_version(void) {
    return caj_xpu_calib_bytes(CAJETA_RT_VERSION, strlen(CAJETA_RT_VERSION));
}

/* The device's architecture: the GPU's arch name, or the host CPU's brand on the cpu backend. */
void* __cajeta_xpu_device_arch(void) {
    char arch[128];
    arch[0] = 0;
    if (cajeta_xpu_active_backend() == CAJ_XPU_CPU) {
#if defined(__x86_64__) || defined(__i386__)
        unsigned int r[12];
        if (__get_cpuid(0x80000000u, &r[0], &r[1], &r[2], &r[3]) && r[0] >= 0x80000004u) {
            for (unsigned int leaf = 0; leaf < 3; leaf++)
                __get_cpuid(0x80000002u + leaf, &r[leaf * 4], &r[leaf * 4 + 1], &r[leaf * 4 + 2], &r[leaf * 4 + 3]);
            memcpy(arch, r, 48);
            arch[48] = 0;
        }
#endif
    } else {
        if (g_xpu_geo_state == 0) {
            g_xpu_geo_state = cajeta_xpu_query_raw_device(&g_xpu_geo) && g_xpu_geo.valid ? 1 : -1;
        }
        if (g_xpu_geo_state == 1) snprintf(arch, sizeof arch, "%s", g_xpu_geo.archName);
    }
    size_t n = strlen(arch);
    size_t s = 0;
    while (s < n && arch[s] == ' ') s++;
    while (n > s && arch[n - 1] == ' ') n--;
    return caj_xpu_calib_bytes(arch + s, n - s);
}

#define CAJ_XPU_CALIB_MAX 256

struct caj_xpu_calib_record {
    char slot[128];
    const uint8_t* bytes;
    int64_t len;
};

static struct caj_xpu_calib_record g_xpu_calib[CAJ_XPU_CALIB_MAX];
static int g_xpu_calib_count = 0;
static pthread_mutex_t g_xpu_calib_lock = PTHREAD_MUTEX_INITIALIZER;

/* Registers one read-only record embedded at build (§12.4c); the bytes must outlive the program. */
void __cajeta_xpu_register_calibration_record(const char* slot, const uint8_t* bytes, int64_t len) {
    if (!slot || !bytes || len <= 0 || strlen(slot) >= sizeof g_xpu_calib[0].slot) return;
    pthread_mutex_lock(&g_xpu_calib_lock);
    int i;
    for (i = 0; i < g_xpu_calib_count; i++) {
        if (strcmp(g_xpu_calib[i].slot, slot) == 0) break;
    }
    if (i < CAJ_XPU_CALIB_MAX) {
        snprintf(g_xpu_calib[i].slot, sizeof g_xpu_calib[i].slot, "%s", slot);
        g_xpu_calib[i].bytes = bytes;
        g_xpu_calib[i].len = len;
        if (i == g_xpu_calib_count) g_xpu_calib_count++;
    }
    pthread_mutex_unlock(&g_xpu_calib_lock);
}

/* The embedded record for `slot`, or NULL. */
void* __cajeta_xpu_calibration_embedded(void* slotArr, int64_t len) {
    if (!slotArr || len <= 0 || len >= 128) return NULL;
    char slot[128];
    memcpy(slot, (const char*) slotArr + 8, (size_t) len);
    slot[len] = 0;
    const uint8_t* bytes = NULL;
    int64_t n = 0;
    pthread_mutex_lock(&g_xpu_calib_lock);
    for (int i = 0; i < g_xpu_calib_count; i++) {
        if (strcmp(g_xpu_calib[i].slot, slot) == 0) {
            bytes = g_xpu_calib[i].bytes;
            n = g_xpu_calib[i].len;
            break;
        }
    }
    pthread_mutex_unlock(&g_xpu_calib_lock);
    if (!bytes) return NULL;
    return caj_xpu_calib_bytes((const char*) bytes, (size_t) n);
}

/* Host memory load for the APU bandwidth probe (§12.1): threads streaming memset until stopped. */
#define CAJ_XPU_HOSTLOAD_MAX 64

static volatile int g_xpu_hostload_run = 0;
static pthread_t g_xpu_hostload_threads[CAJ_XPU_HOSTLOAD_MAX];
static int g_xpu_hostload_count = 0;
static volatile unsigned char g_xpu_hostload_sink = 0;

static void* caj_xpu_hostload_main(void* arg) {
    size_t n = (size_t) 64 << 20;
    unsigned char* buf = (unsigned char*) malloc(n);
    if (!buf) return NULL;
    int v = (int) (intptr_t) arg;
    while (g_xpu_hostload_run) {
        memset(buf, v++ & 255, n);
        g_xpu_hostload_sink ^= buf[(size_t) v * 4099 % n];
    }
    free(buf);
    return NULL;
}

/* Starts `threads` host threads streaming memory (0 = half the host CPUs); returns how many started. */
int32_t __cajeta_xpu_host_load_start(int32_t threads) {
    if (g_xpu_hostload_count > 0) return g_xpu_hostload_count;
    if (threads <= 0) {
#if defined(_SC_NPROCESSORS_ONLN)
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        threads = cpus > 1 ? (int32_t) (cpus / 2) : 1;
#else
        threads = 4;
#endif
    }
    if (threads > CAJ_XPU_HOSTLOAD_MAX) threads = CAJ_XPU_HOSTLOAD_MAX;
    g_xpu_hostload_run = 1;
    for (int i = 0; i < threads; i++) {
        if (pthread_create(&g_xpu_hostload_threads[g_xpu_hostload_count], NULL, caj_xpu_hostload_main,
                           (void*) (intptr_t) i) == 0) {
            g_xpu_hostload_count++;
        }
    }
    return g_xpu_hostload_count;
}

/* Stops the host load threads and joins them. */
void __cajeta_xpu_host_load_stop(void) {
    g_xpu_hostload_run = 0;
    for (int i = 0; i < g_xpu_hostload_count; i++) pthread_join(g_xpu_hostload_threads[i], NULL);
    g_xpu_hostload_count = 0;
}
