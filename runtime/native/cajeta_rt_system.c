// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).

#include <time.h>

// System.exit(code). Terminates the process — caller must not rely on return.
__attribute__((noreturn))
void __cajeta_exit(int32_t code) {
    // _Exit runs no atexit handlers, so the profile trace must be flushed first.
    __cajeta_prof_shutdown();
    _Exit(code);
}

// System.currentTimeMillis(). Wall-clock ms since the Unix epoch.
int64_t __cajeta_currentTimeMillis(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (int64_t) ts.tv_sec * 1000 + (int64_t) (ts.tv_nsec / 1000000);
}

static int32_t cj_tzif_be32(const uint8_t* p) {
    return (int32_t) (((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
                      ((uint32_t) p[2] << 8) | (uint32_t) p[3]);
}

static int64_t cj_tzif_be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | (uint64_t) p[i];
    return (int64_t) v;
}

// Find the utoff (seconds) active at `epoch` in one TZif data block starting at
// `start`; `timesize` is 4 or 8. Bounds-checked against `fsz`; INT32_MIN on error.
static int32_t cj_tzif_offset_at(const uint8_t* buf, size_t fsz, size_t start,
                                 int timesize, uint32_t timecnt, uint32_t typecnt,
                                 int64_t epoch) {
    if (typecnt == 0) return INT32_MIN;
    size_t times = start;
    size_t idxs = times + (size_t) timecnt * (size_t) timesize;
    size_t ttinfo = idxs + (size_t) timecnt;          // typecnt * 6 bytes
    size_t ttend = ttinfo + (size_t) typecnt * 6;
    if (ttend > fsz) return INT32_MIN;

    long sel = -1;
    for (uint32_t i = 0; i < timecnt; i++) {
        int64_t t = (timesize == 8) ? cj_tzif_be64(buf + times + (size_t) i * 8)
                                    : (int64_t) cj_tzif_be32(buf + times + (size_t) i * 4);
        if (t <= epoch) {
            sel = (long) i;
        } else {
            break;
        }
    }

    uint32_t typeIndex;
    if (sel >= 0) {
        typeIndex = buf[idxs + (size_t) sel];
    } else {
        typeIndex = 0;
        for (uint32_t i = 0; i < typecnt; i++) {
            if (buf[ttinfo + (size_t) i * 6 + 4] == 0) {
                typeIndex = i;
                break;
            }
        }
    }
    if (typeIndex >= typecnt) return INT32_MIN;
    return cj_tzif_be32(buf + ttinfo + (size_t) typeIndex * 6);
}

// Total size of a TZif data block following its 44-byte header.
static size_t cj_tzif_block_size(const uint8_t* h, int timesize, int leapsize) {
    uint32_t isutcnt = (uint32_t) cj_tzif_be32(h + 20);
    uint32_t isstdcnt = (uint32_t) cj_tzif_be32(h + 24);
    uint32_t leapcnt = (uint32_t) cj_tzif_be32(h + 28);
    uint32_t timecnt = (uint32_t) cj_tzif_be32(h + 32);
    uint32_t typecnt = (uint32_t) cj_tzif_be32(h + 36);
    uint32_t charcnt = (uint32_t) cj_tzif_be32(h + 40);
    return (size_t) timecnt * timesize + timecnt + (size_t) typecnt * 6 + charcnt +
           (size_t) leapcnt * leapsize + isstdcnt + isutcnt;
}

// Resolve the UTC offset (seconds) for an IANA zone name at `epoch` from the
// system tz database, honoring DST. Returns INT32_MIN when the zone cannot be
// resolved; UTC-equivalent names resolve to 0 without touching the filesystem.
int32_t __cajeta_tz_offset(const void* name_hdr, int64_t name_len, int64_t epoch) {
    if (!name_hdr || name_len <= 0 || name_len > 255) return INT32_MIN;
    const char* name = (const char*) name_hdr + 8;

    if ((name_len == 3 && (memcmp(name, "UTC", 3) == 0 || memcmp(name, "GMT", 3) == 0)) ||
        (name_len == 1 && name[0] == 'Z') ||
        (name_len == 7 && memcmp(name, "Etc/UTC", 7) == 0) ||
        (name_len == 7 && memcmp(name, "Etc/GMT", 7) == 0)) {
        return 0;
    }

    if (name[0] == '/') return INT32_MIN;
    for (int64_t i = 0; i + 1 < name_len; i++) {
        if (name[i] == '.' && name[i + 1] == '.') return INT32_MIN;
    }

    char path[320];
    snprintf(path, sizeof(path), "/usr/share/zoneinfo/%.*s", (int) name_len, name);

    FILE* f = fopen(path, "rb");
    if (!f) return INT32_MIN;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return INT32_MIN; }
    long fsz_l = ftell(f);
    if (fsz_l < 44 || fsz_l > (1 << 22)) { fclose(f); return INT32_MIN; }
    rewind(f);
    size_t fsz = (size_t) fsz_l;
    uint8_t* buf = (uint8_t*) malloc(fsz);
    if (!buf) { fclose(f); return INT32_MIN; }
    if (fread(buf, 1, fsz, f) != fsz) { free(buf); fclose(f); return INT32_MIN; }
    fclose(f);

    int32_t result = INT32_MIN;
    if (memcmp(buf, "TZif", 4) == 0) {
        char ver = (char) buf[4];
        uint32_t timecnt1 = (uint32_t) cj_tzif_be32(buf + 32);
        uint32_t typecnt1 = (uint32_t) cj_tzif_be32(buf + 36);
        size_t v1size = cj_tzif_block_size(buf, 4, 8);
        size_t off1 = 44;

        if ((ver == '2' || ver == '3') && off1 + v1size + 44 <= fsz) {
            size_t h2 = off1 + v1size;
            uint32_t timecnt2 = (uint32_t) cj_tzif_be32(buf + h2 + 32);
            uint32_t typecnt2 = (uint32_t) cj_tzif_be32(buf + h2 + 36);
            result = cj_tzif_offset_at(buf, fsz, h2 + 44, 8, timecnt2, typecnt2, epoch);
        } else if (off1 + v1size <= fsz) {
            result = cj_tzif_offset_at(buf, fsz, off1, 4, timecnt1, typecnt1, epoch);
        }
    }
    free(buf);
    return result;
}

// Math.random() - pseudo-random double in [0.0, 1.0), lazily seeded from the clock.
double __cajeta_random(void) {
    static int seeded = 0;
    if (!seeded) {
        seeded = 1;
        srand((unsigned) __cajeta_currentTimeMillis());
    }
    return (double) rand() / ((double) RAND_MAX + 1.0);
}

// substring(begin, end) for the LEGACY primitive-alias String path (null-terminated
// C-strings). Half-open, indices clamp, result is a freshly malloc'd copy.
char* __cajeta_str_substring(const char* s, int64_t begin, int64_t end) {
    if (!s) {
        char* out = (char*) malloc(1);
        if (out) out[0] = '\0';
        return out;
    }
    int64_t n = (int64_t) strlen(s);
    if (begin < 0) begin = 0;
    if (end > n) end = n;
    if (end < begin) end = begin;
    int64_t len = end - begin;
    char* out = (char*) malloc((size_t) len + 1);
    if (!out) return NULL;
    memcpy(out, s + begin, (size_t) len);
    out[len] = '\0';
    return out;
}

// --- general-purpose hashing (cajeta.hash backend) --------------------------
