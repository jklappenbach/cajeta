// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// --- cajeta.lang.Object root methods ----------------------------------------
// Default bodies for the universal-root methods, overridden per concrete class by the
// structural synthesizer once auto-extend lands; each takes the cajeta `this` unchanged
// as `void* self`. `operator==` is absent: its i1 return lowers to i8 and fails verify.

// Identity hash — the same path as __cajeta_hash_identity.
int64_t __cajeta_object_hash(void* self) {
    return (int64_t) splitmix64_finalize(
        (uint64_t)(uintptr_t) self ^ __cajeta_hash_seed_load());
}

// Placeholder toString, NULL until the String construction surface lands; a caller sees
// a null String. Present so the @Native bridge type-checks.
void* __cajeta_object_to_string(void* self) {
    (void) self;
    return NULL;
}

// Shallow copy plus per-field fixup: a String field becomes a FRESH STAKE on the same
// buffer (no GC, so a shared wrapper would UAF) and a String receiver DETACHES.
void* __cajeta_object_clone(void* self) {
    if (!self) return NULL;
    CajetaRtti* r = (CajetaRtti*) cajeta_rtti_from_obj(self);
    if (!r || r->allocationSize <= 0) return NULL;
    if (r->typeName && strcmp(r->typeName, "cajeta.lang.String") == 0) {
        cajeta_string_layout* s = (cajeta_string_layout*) self;
        int32_t len = caj_str_len(s);
        cajeta_string_layout* out =
            (cajeta_string_layout*) __cajeta_alloc(sizeof(cajeta_string_layout));
        out->vtable = s->vtable;
        out->cachedCpLength = s->cachedCpLength;
        if (len <= CAJ_STR_INLINE_CAP) {
            caj_str_set_inline(out, caj_str_ptr(s), len);
        } else {
            void* buf = caj_str_new_root(caj_str_ptr(s), len);
            caj_str_set_window(out, len, 0, buf);
        }
        return out;
    }
    void* out = __cajeta_alloc((uint64_t) r->allocationSize);
    memcpy(out, self, (size_t) r->allocationSize);
    for (int32_t i = 0; i < (int32_t) r->propertyCount; i++) {
        const CajetaFieldDesc* f = &r->properties[i];
        if (f->byteOffset < 0 || !f->type) continue;
        if (strcmp(f->type, "cajeta.lang.String") == 0) {
            void** slot = (void**) ((char*) out + f->byteOffset);
            void* src = *slot;
            if (src) {
                cajeta_string_layout* fs = (cajeta_string_layout*) src;
                *slot = __cajeta_string_slice(src, 0, caj_str_len(fs));
            }
        } else if (strcmp(f->type, "cajeta.lang.Utf8") == 0) {
            // The memcpy duplicated the inline 16 bytes, so a Shared form needs its own
            // stake. Direct Utf8 fields only — RTTI lists direct fields.
            __cajeta_utf8_retain((char*) out + f->byteOffset);
        }
    }
    return out;
}

// --- parsing helpers --------------------------------------------------------
// All return zero on error (0, false) rather than throwing, so a caller needs no
// try/catch around routine parsing.

int64_t __cajeta_parse_i64(const char* s) {
    if (!s) return 0;
    return (int64_t) strtoll(s, NULL, 10);
}

double __cajeta_parse_f64(const char* s) {
    if (!s) return 0.0;
    return strtod(s, NULL);
}

// Parse a length-bounded, NOT null-terminated buffer as float64, so a JSON float token
// reaches strtod uncopied. A span of 63 bytes or more is truncated.
double __cajeta_strtod_span(const char* s, int64_t len) {
    if (!s || len <= 0) return 0.0;
    char tmp[64];
    if (len >= (int64_t) sizeof(tmp)) len = (int64_t) sizeof(tmp) - 1;
    for (int64_t i = 0; i < len; i++) tmp[i] = s[i];
    tmp[len] = '\0';
    return strtod(tmp, NULL);
}

// Format a float64 into `out` with printf %g semantics. Returns the bytes written,
// excluding the null terminator, or -1 when the buffer was too small.
int32_t __cajeta_format_f64(double v, char* out, int64_t outLen) {
    if (!out || outLen <= 1) return -1;
    int n = snprintf(out, (size_t) outLen, "%.17g", v);
    if (n < 0 || n >= (int) outLen) return -1;
    return (int32_t) n;
}

int32_t __cajeta_parse_bool(const char* s) {
    if (!s) return 0;
    // Case-insensitive match for "true"; everything else is false (Java semantics).
    size_t n = strlen(s);
    if (n != 4) return 0;
    return (((s[0] | 0x20) == 't') && ((s[1] | 0x20) == 'r')
         && ((s[2] | 0x20) == 'u') && ((s[3] | 0x20) == 'e')) ? 1 : 0;
}

// String.valueOf(char) — wraps a single byte as a 1-char malloc'd string.
char* __cajeta_str_fromChar(int8_t c) {
    char* out = (char*) malloc(2);
    if (!out) return NULL;
    out[0] = (char) c;
    out[1] = '\0';
    return out;
}

// Wrap a malloc'd C string into a fresh owned cajeta.lang.String with the caller's
// vtable, freeing the input when `freeIt` — downstream gets a REAL String object.
void* __cajeta_string_wrap_cstr(char* cstr, void* vtable, int32_t freeIt) {
    size_t len = cstr ? strlen(cstr) : 0;
    cajeta_string_layout* out =
        (cajeta_string_layout*) __cajeta_alloc(sizeof(cajeta_string_layout));
    out->vtable = vtable;
    out->cachedCpLength = -1;
    if (len <= (size_t) CAJ_STR_INLINE_CAP) {
        caj_str_set_inline(out, cstr, (int32_t) len);
    } else {
        void* buf = caj_str_new_root(cstr, (int32_t) len);
        caj_str_set_window(out, (int32_t) len, 0, buf);
    }
    // freeIt=0 marks a `.rodata` cstr (bool literals etc.) — never free those.
    if (cstr && freeIt) free(cstr);
    return out;
}

char* __cajeta_str_concat(const char* a, const char* b) {
    if (!a) a = "null";
    if (!b) b = "null";
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char* out = (char*) malloc(la + lb + 1);
    if (!out) return NULL;
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    out[la + lb] = '\0';
    return out;
}

// JSON-quote and escape `data[0..n)` into a malloc'd `"…"` string per RFC 8259 §7. The
// range may hold embedded NULs; a null pointer renders as the bare token `null`.
char* __cajeta_json_quote_buf(const char* data, int64_t n) {
    if (!data) {
        char* out = (char*) malloc(5);
        if (!out) return NULL;
        memcpy(out, "null", 5);
        return out;
    }
    if (n < 0) n = 0;
    // Worst case: every byte becomes `\uXXXX` (6 chars) + 2 outer quotes + NUL.
    size_t cap = (size_t) n * 6 + 3;
    char* out = (char*) malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    out[o++] = '"';
    for (int64_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char) data[i];
        switch (c) {
            case '"':  out[o++] = '\\'; out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\b': out[o++] = '\\'; out[o++] = 'b';  break;
            case '\f': out[o++] = '\\'; out[o++] = 'f';  break;
            case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
            case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
            case '\t': out[o++] = '\\'; out[o++] = 't';  break;
            default:
                if (c < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    out[o++] = '\\'; out[o++] = 'u';
                    out[o++] = '0';  out[o++] = '0';
                    out[o++] = hex[(c >> 4) & 0xF];
                    out[o++] = hex[c & 0xF];
                } else {
                    out[o++] = (char) c;
                }
        }
    }
    out[o++] = '"';
    out[o] = '\0';
    return out;
}

// SLF4J-style format: each `{}` in `fmt` takes argv[i] in order. Extra args are
// dropped, and a `{}` past the end of argv prints "null".
void __cajeta_log(int32_t stream, const char* fmt, int64_t argc, const char* const* argv) {
    if (!fmt) return;
    const char* p = fmt;
    int64_t argIdx = 0;
    while (*p) {
        if (p[0] == '{' && p[1] == '}') {
            const char* arg = (argv && argIdx < argc) ? argv[argIdx] : NULL;
            if (arg) {
                __cajeta_emit(stream, arg, strlen(arg));
            } else {
                __cajeta_emit(stream, "null", 4);
            }
            p += 2;
            argIdx++;
        } else {
            const char* run = p;
            while (*p && !(p[0] == '{' && p[1] == '}')) p++;
            __cajeta_emit(stream, run, (size_t) (p - run));
        }
    }
}

// The `println` variant of __cajeta_log: same `{}` substitution, but always ends the
// line with one '\n' whatever the format string ended with.
void __cajeta_logln(int32_t stream, const char* fmt, int64_t argc, const char* const* argv) {
    __cajeta_log(stream, fmt, argc, argv);
    __cajeta_emit(stream, "\n", 1);
}

// ---------------------------------------------------------------------------
// System.env — OS environment variables; System.property — process-scoped string
// properties. Both trade in char*: the codegen wraps one into a class String, NULL null.

// getenv's pointer lives in the environ strip and dies on the next setenv/putenv, so
// the codegen-side wrap copies the bytes; this hands back the libc pointer as it is.
const char* __cajeta_env_get(const char* name) {
    if (!name) return NULL;
    return getenv(name);
}

int32_t __cajeta_env_set(const char* name, const char* value) {
    if (!name) return -1;
#if defined(_WIN32)
    // The Windows CRT's _putenv_s unsets by taking "" as the value.
    if (!value) {
        return _putenv_s(name, "") == 0 ? 0 : -1;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
#else
    if (!value) {
        // setenv with NULL value is undefined on some libcs; treat as unset.
        return unsetenv(name);
    }
    return setenv(name, value, /*overwrite=*/1);
#endif
}

struct cajeta_property_entry {
    char* key;
    char* value;
    struct cajeta_property_entry* next;
};

static struct cajeta_property_entry* __cajeta_property_head = NULL;
static pthread_mutex_t __cajeta_property_mu = PTHREAD_MUTEX_INITIALIZER;

// A process-global linear key→value list. Keys and values are strdup'd copies, so a
// caller's pointers need not outlive the call, and a mutex guards the shared head.
const char* __cajeta_property_get(const char* name) {
    if (!name) return NULL;
    pthread_mutex_lock(&__cajeta_property_mu);
    for (struct cajeta_property_entry* e = __cajeta_property_head; e; e = e->next) {
        if (e->key && strcmp(e->key, name) == 0) {
            pthread_mutex_unlock(&__cajeta_property_mu);
            return e->value;
        }
    }
    pthread_mutex_unlock(&__cajeta_property_mu);
    return NULL;
}

void __cajeta_property_set(const char* name, const char* value) {
    if (!name) return;
    pthread_mutex_lock(&__cajeta_property_mu);
    for (struct cajeta_property_entry* e = __cajeta_property_head; e; e = e->next) {
        if (e->key && strcmp(e->key, name) == 0) {
            free(e->value);
            e->value = value ? strdup(value) : NULL;
            pthread_mutex_unlock(&__cajeta_property_mu);
            return;
        }
    }
    struct cajeta_property_entry* e = (struct cajeta_property_entry*) malloc(sizeof(*e));
    if (!e) {
        pthread_mutex_unlock(&__cajeta_property_mu);
        return;
    }
    e->key = strdup(name);
    e->value = value ? strdup(value) : NULL;
    e->next = __cajeta_property_head;
    __cajeta_property_head = e;
    pthread_mutex_unlock(&__cajeta_property_mu);
}

// Parse a `key=value` token and install it — the C-main shim's `-Dkey=value` argv walk.
// A token with no `=`, like an explicitly empty one, installs "".
void __cajeta_property_install(const char* keyEqValue) {
    if (!keyEqValue) return;
    const char* eq = strchr(keyEqValue, '=');
    if (!eq) {
        __cajeta_property_set(keyEqValue, "");
        return;
    }
    size_t keyLen = (size_t) (eq - keyEqValue);
    if (keyLen == 0) return;
    char* key = (char*) malloc(keyLen + 1);
    if (!key) return;
    memcpy(key, keyEqValue, keyLen);
    key[keyLen] = '\0';
    __cajeta_property_set(key, eq + 1);
    free(key);
}

// ---------------------------------------------------------------------------
// The process argument vector (`System.args`). ONE STORE, TWO SPELLINGS: a `main(String[]
// args)` and `System.args` are fed by the same install call, so they cannot disagree. The
// strings are COPIED — a host's backing may die first, and a dangling argv reads as data.
static char**  __cajeta_argv_store = NULL;
static int64_t __cajeta_argc_store = 0;
static pthread_mutex_t __cajeta_args_mu = PTHREAD_MUTEX_INITIALIZER;

void __cajeta_args_install(int64_t argc, char** argv) {
    if (argc < 0) argc = 0;
    pthread_mutex_lock(&__cajeta_args_mu);
    // Re-installing replaces: free the previous copy rather than leak it.
    if (__cajeta_argv_store) {
        for (int64_t i = 0; i < __cajeta_argc_store; i++) free(__cajeta_argv_store[i]);
        free(__cajeta_argv_store);
        __cajeta_argv_store = NULL;
    }
    __cajeta_argc_store = 0;
    if (argc > 0) {
        __cajeta_argv_store = (char**) malloc((size_t) argc * sizeof(char*));
        if (!__cajeta_argv_store) {
            pthread_mutex_unlock(&__cajeta_args_mu);
            return;
        }
        for (int64_t i = 0; i < argc; i++) {
            const char* s = (argv && argv[i]) ? argv[i] : "";
            __cajeta_argv_store[i] = strdup(s);
        }
        __cajeta_argc_store = argc;
    }
    pthread_mutex_unlock(&__cajeta_args_mu);
}

int64_t __cajeta_args_count(void) {
    pthread_mutex_lock(&__cajeta_args_mu);
    int64_t n = __cajeta_argc_store;
    pthread_mutex_unlock(&__cajeta_args_mu);
    return n;
}

// Out of range yields NULL, which the cajeta side wraps as a null String — the shape
// `System.env.get` uses. "" would make a typo'd index look like an empty argument.
const char* __cajeta_args_get(int64_t index) {
    pthread_mutex_lock(&__cajeta_args_mu);
    const char* out = NULL;
    if (index >= 0 && index < __cajeta_argc_store && __cajeta_argv_store) {
        out = __cajeta_argv_store[index];
    }
    pthread_mutex_unlock(&__cajeta_args_mu);
    return out;
}

// Publish the host's release target triple as `cajeta.host.triple`, so a program can pick
// its release asset with no -D or exec. Unknown arch/OS leaves it unset; set before main.
__attribute__((constructor))
static void __cajeta_install_host_triple(void) {
#if defined(_WIN32)
    __cajeta_property_set("cajeta.host.triple", "x86_64-w64-mingw32");
#else
    struct utsname u;
    if (uname(&u) != 0) return;
    const char* os = NULL;
    if (strcmp(u.sysname, "Linux") == 0)       os = "linux-gnu";
    else if (strcmp(u.sysname, "Darwin") == 0) os = "apple-darwin";
    if (!os) return;
    const char* arch = NULL;
    if (strcmp(u.machine, "x86_64") == 0)      arch = "x86_64";
    else if (strcmp(u.machine, "aarch64") == 0
          || strcmp(u.machine, "arm64") == 0)  arch = "aarch64";
    if (!arch) return;
    char triple[64];
    snprintf(triple, sizeof(triple), "%s-%s", arch, os);
    __cajeta_property_set("cajeta.host.triple", triple);
#endif
}

// ---------------------------------------------------------------------------
// cajeta.io.file — one-shot reads and writes plus the streaming open/read/write/close
// set. The one-shot helpers materialize a CajetaArray header (int64 count + raw bytes)
// in the live set; streaming takes raw fds, where 0 is EOF and negative a hard error.
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(_WIN32)
#  include <io.h>   // _fstat64 / _lseeki64 / _chsize_s — the 64-bit CRT forms
#endif

// 64-bit file metadata everywhere. The mingw stat/off_t family is 32-bit unless
// _FILE_OFFSET_BITS=64 precedes the first include, which a #included fragment cannot do.
static int cajeta_file_stat64(int fd, int64_t* size, bool* isRegular) {
#if defined(_WIN32)
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0) return -1;
    if (size) *size = (int64_t) st.st_size;
    if (isRegular) *isRegular = (st.st_mode & _S_IFMT) == _S_IFREG;
#else
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (size) *size = (int64_t) st.st_size;
    if (isRegular) *isRegular = S_ISREG(st.st_mode);
#endif
    return 0;
}

// One read/_read call's ceiling: Linux caps a single read at 0x7ffff000 and the Windows
// CRT rejects counts past INT_MAX, so 1 GiB is inside every platform's limit.
#define CAJETA_FILE_READ_CHUNK ((int64_t) 0x40000000)

// open() defaults to TEXT mode on Windows/MinGW, translating \n <-> \r\n and silently
// corrupting binary payloads, so force O_BINARY; POSIX has no text mode, so it is 0.
#ifndef O_BINARY
#define O_BINARY 0
#endif

// The int32 ordinal must mirror runtime/src/cajeta/io/file/OpenMode.cajeta:
// 0 READ, 1 WRITE, 2 APPEND, 3 READ_WRITE, 4 CREATE_NEW.
static int __cajeta_file_mode_to_flags(int32_t mode) {
    switch (mode) {
        case 0: return O_RDONLY | O_BINARY;
        case 1: return O_WRONLY | O_CREAT | O_TRUNC | O_BINARY;
        case 2: return O_WRONLY | O_CREAT | O_APPEND | O_BINARY;
        case 3: return O_RDWR | O_CREAT | O_BINARY;
        case 4: return O_WRONLY | O_CREAT | O_EXCL | O_BINARY;
        default: return O_RDONLY | O_BINARY;
    }
}

// Read the whole file at `path` into a live-set CajetaArray header; NULL on any failure.
void* __cajeta_file_read_all(const char* path) {
    if (!path) return NULL;
    int fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) return NULL;

    int64_t size = 0;
    bool isRegular = false;
    if (cajeta_file_stat64(fd, &size, &isRegular) != 0 || !isRegular) {
        close(fd);
        return NULL;
    }
    if (size < 0) size = 0;

    // Header layout matches __cajeta_new_array_header: 8-byte count, then raw bytes.
    void* hdr = __cajeta_new_array_header(8, 1, (uint64_t) size);
    if (!hdr) {
        close(fd);
        return NULL;
    }
    char* data = ((char*) hdr) + 8;
    int64_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, data + got, (size_t) (size - got));
        if (n < 0) {
            if (errno == EINTR) continue;
            // Leave the partial header in the live set; the scope drop reclaims it.
            close(fd);
            __cajeta_free_array(hdr);
            return NULL;
        }
        if (n == 0) break;  // EOF earlier than stat reported; truncate.
        got += (int64_t) n;
    }
    close(fd);
    // A writer truncating mid-call leaves us short: shrink the count word so a
    // caller's `count()` matches what is actually populated.
    if (got != size) {
        *((int64_t*) hdr) = got;
    }
    return hdr;
}

// Write `len` bytes of `data` to `path` atomically: `<path>.tmp.<pid>`, fsync, rename,
// unlinking the tmp on failure. 0 or -1; `len` is int64, so a multi-GiB payload survives.
int32_t __cajeta_file_write_all(const char* path, const void* data, int64_t len) {
    if (!path || len < 0) return -1;
    if (!data && len > 0) return -1;

    // Bounded buffer: reject a path whose tmp form would overflow (~32-char headroom).
    char tmp[4096];
    size_t pathLen = strlen(path);
    if (pathLen + 32 >= sizeof(tmp)) return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int) getpid());

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fd < 0) return -1;
    int64_t remaining = len;
    const char* p = (const char*) data;
    while (remaining > 0) {
        // write(2) caps a single call (Linux: 2^31-4096); loop past partials.
        ssize_t n = write(fd, p, (size_t) remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp);
            return -1;
        }
        p += n;
        remaining -= (int64_t) n;
    }
    // Best-effort fsync; some filesystems treat it as a no-op.
    fsync(fd);
    close(fd);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

// Streaming open: the POSIX fd on success, -1 on failure. The cajeta side wraps the fd
// into a FileReader / FileWriter instance.
int32_t __cajeta_file_open(const char* path, int32_t mode) {
    if (!path) return -1;
    int flags = __cajeta_file_mode_to_flags(mode);
    int fd;
    // An O_CREAT'd open needs the perm-bit arg; other modes ignore it.
    fd = open(path, flags, 0644);
    return fd;  // -1 on failure (errno set; caller can translate).
}

// Fill up to `max` bytes of `buf`, the int8[]'s data region, returning the count filled:
// 0 is EOF, negative a hard error. int64, since an int32 bit-31 length read as EOF.
int64_t __cajeta_file_read(int32_t fd, void* buf, int64_t max) {
    if (fd < 0 || !buf || max <= 0) return 0;
    // Streams (pipes, sockets, ttys) must return as soon as ANY bytes are available, or
    // an interactive peer blocks forever; only regular files keep the fill-to-max loop.
    bool isRegular = false;
    bool fillToMax = (cajeta_file_stat64(fd, NULL, &isRegular) == 0 && isRegular);
    if (!fillToMax) {
        ssize_t n;
        int64_t want = max < CAJETA_FILE_READ_CHUNK ? max : CAJETA_FILE_READ_CHUNK;
        do {
            n = read(fd, buf, (size_t) want);
        } while (n < 0 && errno == EINTR);
        if (n < 0) return -1;
        return (int64_t) n;  // 0 == EOF; otherwise bytes available now.
    }
    int64_t got = 0;
    while (got < max) {
        int64_t left = max - got;
        int64_t want = left < CAJETA_FILE_READ_CHUNK ? left : CAJETA_FILE_READ_CHUNK;
        ssize_t n = read(fd, ((char*) buf) + got, (size_t) want);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;  // EOF.
        got += (int64_t) n;
    }
    return got;
}

// Streaming write, looping past partial writes: returns the count written, `len` on
// success, -1 on a hard error. int64 end to end, like the read side.
int64_t __cajeta_file_write(int32_t fd, const void* data, int64_t len) {
    if (fd < 0 || len < 0) return -1;
    if (!data && len > 0) return -1;
    const char* p = (const char*) data;
    int64_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, (size_t) remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        remaining -= (int64_t) n;
    }
    return len;
}

// Random-access File helpers: the seek / size / truncate / lock primitives. The
// streaming set above is re-used as-is by the random-access File.

// `whence`: 0 SEEK_SET, 1 SEEK_CUR, 2 SEEK_END. Returns the new
// absolute position, or -1 on failure.
int64_t __cajeta_file_seek(int32_t fd, int64_t offset, int32_t whence) {
    if (fd < 0) return -1;
    int w = SEEK_SET;
    if (whence == 1) w = SEEK_CUR;
    else if (whence == 2) w = SEEK_END;
#if defined(_WIN32)
    // off_t is 32-bit on mingw; _lseeki64 is the CRT's 64-bit seek.
    __int64 r = _lseeki64(fd, (__int64) offset, w);
#else
    off_t r = lseek(fd, (off_t) offset, w);
#endif
    return (int64_t) r;
}

int64_t __cajeta_file_size_of(int32_t fd) {
    if (fd < 0) return -1;
    int64_t size = -1;
    if (cajeta_file_stat64(fd, &size, NULL) != 0) return -1;
    return size;
}

int32_t __cajeta_file_truncate(int32_t fd, int64_t size) {
    if (fd < 0 || size < 0) return -1;
#if defined(_WIN32)
    // ftruncate/off_t are 32-bit on mingw; _chsize_s takes a 64-bit size.
    return _chsize_s(fd, (__int64) size) == 0 ? 0 : -1;
#else
    return ftruncate(fd, (off_t) size) == 0 ? 0 : -1;
#endif
}

int32_t __cajeta_file_sync(int32_t fd) {
    if (fd < 0) return -1;
    return fsync(fd) == 0 ? 0 : -1;
}

// POSIX flock locking; MinGW-w64 ships no flock, so map to Win32 LockFileEx.
#if defined(_WIN32)
#  include <io.h>
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
int32_t __cajeta_file_lock(int32_t fd) {
    if (fd < 0) return -1;
    HANDLE h = (HANDLE) _get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov = {0};
    return LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov)
        ? 0 : -1;
}
int32_t __cajeta_file_try_lock(int32_t fd) {
    if (fd < 0) return 0;
    HANDLE h = (HANDLE) _get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    OVERLAPPED ov = {0};
    return LockFileEx(h,
        LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
        0, MAXDWORD, MAXDWORD, &ov) ? 1 : 0;
}
int32_t __cajeta_file_unlock(int32_t fd) {
    if (fd < 0) return -1;
    HANDLE h = (HANDLE) _get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov = {0};
    return UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov) ? 0 : -1;
}
#else
#include <sys/file.h>
int32_t __cajeta_file_lock(int32_t fd) {
    if (fd < 0) return -1;
    return flock(fd, LOCK_EX) == 0 ? 0 : -1;
}

int32_t __cajeta_file_try_lock(int32_t fd) {
    if (fd < 0) return 0;
    return flock(fd, LOCK_EX | LOCK_NB) == 0 ? 1 : 0;
}

int32_t __cajeta_file_unlock(int32_t fd) {
    if (fd < 0) return -1;
    return flock(fd, LOCK_UN) == 0 ? 0 : -1;
}
#endif

// Streaming flush. FileWriter writes straight through today, so this is a no-op stub
// until an internal buffer lands.
int32_t __cajeta_file_flush(int32_t fd) {
    (void) fd;
    return 0;
}

// Close the fd. Passing -1 is a no-op, which is what lets the cajeta-side `close()`
// be idempotent (it sets this.fd = -1 after the first call).
void __cajeta_file_close(int32_t fd) {
    if (fd < 0) return;
    close(fd);
}

// ---------------------------------------------------------------------------
// Memory-mapped files — MappedFile's instance @Native seam: read-only, whole-file, and
// offset 0, so each takes the forwarded `this` first. map returns the base address or 0.
#if defined(_WIN32)
int64_t __cajeta_file_map(void* self, int32_t fd, int64_t length) {
    (void) self;
    if (fd < 0 || length <= 0) return 0;
    HANDLE h = (HANDLE) _get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    HANDLE sec = CreateFileMappingA(h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!sec) return 0;
    void* base = MapViewOfFile(sec, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(sec);   // the view keeps the section alive
    return (int64_t)(intptr_t) base;
}

void __cajeta_file_unmap(void* self, int64_t base, int64_t length) {
    (void) self;
    (void) length;      // UnmapViewOfFile releases the whole view
    if (base) UnmapViewOfFile((void*)(intptr_t) base);
}
#else
#include <sys/mman.h>
int64_t __cajeta_file_map(void* self, int32_t fd, int64_t length) {
    (void) self;
    if (fd < 0 || length <= 0) return 0;
    // MAP_PRIVATE read-only: pages fault in on demand, so a mapping larger than RAM
    // stays only as resident as its touched pages. No MAP_POPULATE, ever.
    void* base = mmap(NULL, (size_t) length, PROT_READ, MAP_PRIVATE, fd, 0);
    if (base == MAP_FAILED) return 0;
    return (int64_t)(intptr_t) base;
}

void __cajeta_file_unmap(void* self, int64_t base, int64_t length) {
    (void) self;
    if (base && length > 0) munmap((void*)(intptr_t) base, (size_t) length);
}
#endif

// Single byte at `off`, zero-extended (0..255); -1 on a dead mapping.
int32_t __cajeta_file_map_byte(void* self, int64_t base, int64_t off) {
    (void) self;
    if (!base || off < 0) return -1;
    return (int32_t) *((const unsigned char*)(intptr_t) base + off);
}

// Bulk copy out of the mapping to a RAW destination address, whose bounds the caller
// owns; the source window is bounds-checked cajeta-side against the mapping length.
int64_t __cajeta_file_map_copy(void* self, int64_t base, int64_t off,
                               int64_t dstAddr, int64_t n) {
    (void) self;
    if (!base || !dstAddr || off < 0 || n < 0) return -1;
    memcpy((void*)(intptr_t) dstAddr,
           (const char*)(intptr_t) base + off, (size_t) n);
    return n;
}

// Bulk copy into a cajeta int8[]. `dstArr` is the array HEADER ({ i64 count, data... }),
// elements at +8, and its count word backstops the destination bound.
int64_t __cajeta_file_map_read(void* self, int64_t base, int64_t off,
                               void* dstArr, int64_t dstOff, int64_t n) {
    (void) self;
    if (!base || !dstArr || off < 0 || dstOff < 0 || n < 0) return -1;
    int64_t dstCount = *((const int64_t*) dstArr);
    if (dstCount < 0) dstCount = dstCount & 0x7FFFFFFFFFFFFFFFLL; // shared-tag bit
    if (dstOff + n > dstCount) return -1;
    memcpy((char*) dstArr + 8 + dstOff,
           (const char*)(intptr_t) base + off, (size_t) n);
    return n;
}

// ===========================================================================
