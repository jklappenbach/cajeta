// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// --- cajeta.lang.Object root methods ----------------------------------------
// Default bodies for the universal-root methods. These are stubs the
// compiler-side structural synthesizer will override per concrete class
// later; until then they're the live implementations any caller would see
// if `extends Object` were already implicit (it isn't yet — see the next
// implementation cut). Keeping them functional now means once auto-extend
// lands, every existing class without manual overrides immediately has
// usable hash() / toString() / clone() — no further runtime changes
// required.
//
// `operator==(Object)` is intentionally absent from this batch. Its
// LLVM return type is i1 (cajeta boolean), but C `_Bool` / `int8_t`
// lowers to i8, and the bitcode-runtime link step overrides the
// @Native bridge's i1 declaration with i8 — producing a `ret i1 of
// i8` verifier failure. Re-enabling it needs either return-type
// coercion in Method::emitNativeForwardingBody or a cajeta-source body
// using `this == other` pointer-equality. Tracked for the next cut.
//
// All functions take `void* this` as the first parameter — the @Native
// bridge in Method::emitNativeForwardingBody passes the cajeta `this`
// pointer through unchanged, matching the cajeta class type's pointer
// ABI.

// Identity hash — same path as __cajeta_hash_identity. Once the
// synthesizer lands, each class's emitted hash() body replaces this call
// with a field-walk; until then, every object key in a HashMap behaves
// the same way Java's default Object.hashCode() does (identity-keyed).
int64_t __cajeta_object_hash(void* self) {
    return (int64_t) splitmix64_finalize(
        (uint64_t)(uintptr_t) self ^ __cajeta_hash_seed_load());
}

// Placeholder toString — returns NULL until the String construction
// surface lands. The structural synthesizer (when it arrives) will emit
// per-class bodies that build "TypeName(field=value, ...)" via the
// cajeta.lang.String stdlib. Callers that hit this stub today get a
// null String, which is the same behaviour they'd have gotten before
// Object existed — i.e. nothing currently calls it, since no class
// inherits Object yet. The stub is here so the @Native bridge type-
// checks; the day a class actually inherits it, the synthesizer will
// be the live implementation.
void* __cajeta_object_to_string(void* self) {
    (void) self;
    return NULL;
}

// Placeholder clone — same stub-with-NULL pattern as toString. The
// field-walking memcpy/shallow-ref-copy implementation lands with the
// synthesizer; until then, manual clone() overrides (or just avoiding
// the call) are the workaround.
void* __cajeta_object_clone(void* self) {
    (void) self;
    return NULL;
}

// --- parsing helpers --------------------------------------------------------
// All return on error: 0 (for numeric forms) and false (for boolean). The
// stdlib spec for Cajeta will likely tighten this to a thrown exception once
// the exception ABI carries class info; for now zero-on-error keeps callers
// from needing a try/catch around routine parsing.

int64_t __cajeta_parse_i64(const char* s) {
    if (!s) return 0;
    return (int64_t) strtoll(s, NULL, 10);
}

double __cajeta_parse_f64(const char* s) {
    if (!s) return 0.0;
    return strtod(s, NULL);
}

// Span variant — parse a length-bounded buffer (not null-terminated)
// as float64. JsonReader uses tokenStart/tokenEnd offsets into the
// input buffer; this lets the float-token path call into the system
// strtod without copying the span first when it fits in a small
// stack buffer. Buffers >= 63 bytes are truncated (an absurdly long
// JSON number is malformed anyway; we surface 0.0).
double __cajeta_strtod_span(const char* s, int64_t len) {
    if (!s || len <= 0) return 0.0;
    char tmp[64];
    if (len >= (int64_t) sizeof(tmp)) len = (int64_t) sizeof(tmp) - 1;
    for (int64_t i = 0; i < len; i++) tmp[i] = s[i];
    tmp[len] = '\0';
    return strtod(tmp, NULL);
}

// Format a float64 into a caller-supplied byte buffer using printf %g
// semantics (shortest round-trip-safe form). Returns the number of
// bytes written (excluding the implicit null terminator), or -1 if
// the buffer was too small. JsonWriter uses this to materialize
// float64 tokens into its growing output buffer.
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

// JSON-quote and escape `data[0..n)` into a freshly-malloc'd
// null-terminated string of the form `"…escaped…"`. Escapes per
// RFC 8259 §7: `"` → `\"`, `\` → `\\`, control chars (0x00..0x1F)
// → `\uXXXX` (with short forms `\b \f \n \r \t` for the common
// five). The data range is allowed to contain embedded NULs.
// A null data pointer renders as the literal token `null` (no
// quotes) so callers don't need a separate null-check arm.
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

// SLF4J-style format: each `{}` in `fmt` is replaced in order by argv[i]'s
// null-terminated string. Extra args after all `{}`s are dropped. Missing
// args (more `{}`s than argv entries) print "null".
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
            // Emit one run of literal text up to the next `{}` (or end).
            const char* run = p;
            while (*p && !(p[0] == '{' && p[1] == '}')) p++;
            __cajeta_emit(stream, run, (size_t) (p - run));
        }
    }
}

// `println` variant of __cajeta_log — same `{}`-substitution semantics, but
// terminate the line with a single '\n' regardless of whether the format
// string ended with one. Mirrors __cajeta_println's relationship to
// __cajeta_print. The codegen routes
// `System.stdout.println(fmt, arg1, arg2, ...)` here so format-with-args
// printing doesn't need a trailing "\n" in the format string.
void __cajeta_logln(int32_t stream, const char* fmt, int64_t argc, const char* const* argv) {
    __cajeta_log(stream, fmt, argc, argv);
    __cajeta_emit(stream, "\n", 1);
}

// ---------------------------------------------------------------------------
// System.env — OS environment-variable access.
// System.property — process-scoped string properties (Java -Dkey=value).
//
// Both surfaces store / return char* values. The cajeta codegen wraps a
// returned char* into a class String at the use site; callers see normal
// String values. NULL returns from `get` map to a null String reference
// at the cajeta level.
//
// `env` is a thin wrapper over libc getenv / setenv. The returned char*
// for getenv points into the process's environ strip and is invalidated by
// subsequent setenv / putenv calls — so the codegen-side wrap copies the
// bytes into a fresh cajeta int8[] header before exposing them. The
// __cajeta_env_get helper here just returns the libc pointer; callers
// are responsible for copying.
//
// `property` is an in-process key→value map. Lookups are linear over a
// singly-linked list of entries; insert/update walks the list and either
// rewrites the value or appends. Both keys and values are heap-allocated
// strdup'd copies so caller pointers don't have to outlive the call. A
// shared mutex protects concurrent set/get from racing on the list head
// (cajeta's parallel-driver workers can hit property accessors from
// multiple fibers; the same single-carrier scheduler runs them today but
// the property map is process-global, so locking is the right shape for
// when multi-carrier lands).
// ---------------------------------------------------------------------------

const char* __cajeta_env_get(const char* name) {
    if (!name) return NULL;
    return getenv(name);
}

int32_t __cajeta_env_set(const char* name, const char* value) {
    if (!name) return -1;
#if defined(_WIN32)
    // Windows CRT uses _putenv_s: pass "" as the value to unset. Returns
    // 0 on success, errno on failure.
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
    // Update in place if key exists.
    for (struct cajeta_property_entry* e = __cajeta_property_head; e; e = e->next) {
        if (e->key && strcmp(e->key, name) == 0) {
            free(e->value);
            e->value = value ? strdup(value) : NULL;
            pthread_mutex_unlock(&__cajeta_property_mu);
            return;
        }
    }
    // Insert at head.
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

// Parse a `key=value` string and install it via __cajeta_property_set.
// Used by the C-main shim (Compiler::emitCMainShim's argv walk) to honor
// `-Dkey=value` CLI args at program startup, mirroring Java's
// `java -Dkey=value …` convention. A token without `=` installs an empty
// string value (`-Dflag` sets `flag` to ""); explicit empty
// (`-Dflag=`) is the same.
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

// Publish the host's release target triple as the `cajeta.host.triple`
// system property at startup, so programs (notably cvm, the version
// manager) can pick the matching release asset WITHOUT a build-time -D or
// a process exec. POSIX maps uname's machine+sysname into the release
// triple vocabulary; Windows is fixed to the single supported MinGW
// target. Unknown arch/OS leaves the property unset (the reader degrades
// to "unknown host" rather than a wrong guess). Runs as a startup
// constructor — placed after __cajeta_property_set so no forward decl is
// needed; main() runs after every constructor, so the property is set by
// the time any user code reads it.
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
// cajeta.io.file — Phase A runtime helpers.
//
// One-shot reads / writes plus the streaming open / read / write / close /
// flush set. POSIX-only today; Windows variants land alongside the
// `_w_*` symbol pairs once the Watcher work motivates them.
//
// The one-shot helpers materialize a CajetaArray header (int64 count
// prefix + raw bytes) so the result drops into the existing array-drop
// chain without special casing. `__cajeta_live_set_add` registers the
// allocation as the unique live owner so the auto-drop helper
// (`__cajeta_free_array`) reclaims it once the caller's scope exits.
//
// Streaming helpers operate on raw `int32` POSIX fds. EOF is reported to
// the caller via a 0 return from `__cajeta_file_read` (matching the
// `read(2)` convention); a negative return signals a hard error that the
// caller-side codegen will eventually translate to an IoException throw
// (the throw machinery is a Phase C follow-up — Phase A just propagates
// the error as a negative count, which the cajeta-side wrapper will
// promote to a thrown exception once the hierarchy lands).
// ---------------------------------------------------------------------------
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// On Windows/MinGW, open() defaults to TEXT mode, which translates
// \n <-> \r\n on read and write. That silently corrupts binary payloads
// (e.g. the LtmBPlusTree pager's fixed-size index pages), and the damage
// only surfaces after a real disk round-trip — a close + cold reopen reads
// back garbage offsets/sizes and aborts. Every file the runtime touches is
// binary, so force O_BINARY. It's undefined on POSIX, where open() has no
// text mode; define it to 0 there so the bitwise-or is a no-op.
#ifndef O_BINARY
#define O_BINARY 0
#endif

// File-open mode enum — must mirror runtime/src/cajeta/io/file/OpenMode.cajeta
// ordinal order. Caller passes the ordinal as int32.
//   0 READ       — O_RDONLY
//   1 WRITE      — O_WRONLY | O_CREAT | O_TRUNC
//   2 APPEND     — O_WRONLY | O_CREAT | O_APPEND
//   3 READ_WRITE — O_RDWR   | O_CREAT
//   4 CREATE_NEW — O_WRONLY | O_CREAT | O_EXCL
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

// `path` is a null-terminated C string (the cajeta-side unwrap step
// strips class String → bytes ptr → +8 past the count word, so what
// arrives here is a real C string with no header prefix).
//
// Returns a CajetaArray header pointer (`{ int64 count, int8[count]
// data }`) whose data region is the entire file contents. The header
// joins the live-set so the caller's scope-exit drop frees it.
//
// On open/stat/read failure, returns NULL — the cajeta-side wrapper
// throws IoException once the hierarchy lands; today, NULL surfaces
// up the chain.
void* __cajeta_file_read_all(const char* path) {
    if (!path) return NULL;
    int fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }
    int64_t size = (int64_t) st.st_size;
    if (size < 0) size = 0;

    // Header layout matches __cajeta_new_array_header: 8-byte count +
    // raw bytes. Element size is 1 (int8).
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
            // Mid-read failure: leave the partial header in the live
            // set; the caller's scope drop reclaims it. Surface NULL.
            close(fd);
            __cajeta_free_array(hdr);
            return NULL;
        }
        if (n == 0) break;  // EOF earlier than stat reported; truncate.
        got += (int64_t) n;
    }
    close(fd);
    // If we read less than expected (race against another writer
    // truncating mid-call), shrink the count word so callers' `count()`
    // matches what's actually populated.
    if (got != size) {
        *((int64_t*) hdr) = got;
    }
    return hdr;
}

// `data` points at the raw bytes (the cajeta-side passes
// arrayPtr + 8 — past the count word). `len` is the number of bytes
// to write. Atomic-rename semantics: write to `<path>.tmp.<pid>`,
// fsync, then rename over `path`. On any failure the tmp file is
// removed; the destination is either pre-write or fully post-write.
//
// Returns 0 on success, -1 on failure.
int32_t __cajeta_file_write_all(const char* path, const void* data, int32_t len) {
    if (!path || len < 0) return -1;
    if (!data && len > 0) return -1;

    // Build the tmp path. Bounded buffer; reject paths whose tmp form
    // would overflow (rare, ~32-char headroom).
    char tmp[4096];
    size_t pathLen = strlen(path);
    if (pathLen + 32 >= sizeof(tmp)) return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int) getpid());

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fd < 0) return -1;
    int32_t remaining = len;
    const char* p = (const char*) data;
    while (remaining > 0) {
        ssize_t n = write(fd, p, (size_t) remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp);
            return -1;
        }
        p += n;
        remaining -= (int32_t) n;
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

// Streaming open. Returns the POSIX fd as int32 on success, -1 on
// failure. Cajeta-side wraps the fd into a FileReader / FileWriter
// instance.
int32_t __cajeta_file_open(const char* path, int32_t mode) {
    if (!path) return -1;
    int flags = __cajeta_file_mode_to_flags(mode);
    int fd;
    // O_CREAT'd opens need a mode arg (perm bits). Modes without
    // O_CREAT ignore the third arg per the glibc prototype but pass
    // 0644 anyway — silently ignored.
    fd = open(path, flags, 0644);
    return fd;  // -1 on failure (errno set; caller can translate).
}

// Streaming read. Fills up to `max` bytes into `buf`. Returns the
// count actually filled. Zero == EOF. Negative == hard error.
//
// `buf` is the cajeta-side int8[]'s data region — caller passes
// `&data[0]`, which is `arrayPtr + 8` past the count word.
int32_t __cajeta_file_read(int32_t fd, void* buf, int32_t max) {
    if (fd < 0 || !buf || max <= 0) return 0;
    // Streams (pipes, sockets, FIFOs, ttys) must return as soon as ANY
    // bytes are available: looping to fill `max` would block an interactive
    // peer (e.g. a line-delimited JSON-RPC client over stdio) forever waiting
    // for bytes it will never send until it sees our reply. Only regular
    // files keep the fill-to-max loop — there a caller asking for `max` past
    // a short read wants the rest up to EOF. "Fills up to max" already permits
    // a short return, so streaming readers loop on the count themselves.
    struct stat st;
    bool fillToMax = (fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
    if (!fillToMax) {
        ssize_t n;
        do {
            n = read(fd, buf, (size_t) max);
        } while (n < 0 && errno == EINTR);
        if (n < 0) return -1;
        return (int32_t) n;  // 0 == EOF; otherwise bytes available now.
    }
    int32_t got = 0;
    while (got < max) {
        ssize_t n = read(fd, ((char*) buf) + got, (size_t) (max - got));
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;  // EOF.
        got += (int32_t) n;
    }
    return got;
}

// Streaming write. Loops past partial writes; returns 0 on success,
// -1 on hard error.
int32_t __cajeta_file_write(int32_t fd, const void* data, int32_t len) {
    if (fd < 0 || len < 0) return -1;
    if (!data && len > 0) return -1;
    const char* p = (const char*) data;
    int32_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, (size_t) remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        remaining -= (int32_t) n;
    }
    return 0;
}

// Phase E — random-access File helpers.
//
// Seek / position / size / truncate / lock — these are all the
// fd-shaped operations the random-access `cajeta.io.file.File`
// class needs. The streaming-only `__cajeta_file_*` helpers
// above (read/write/close/flush) ARE re-used by the random-
// access File; only the seek/lock/truncate primitives are new
// here.

// `whence`: 0 SEEK_SET, 1 SEEK_CUR, 2 SEEK_END. Returns the new
// absolute position, or -1 on failure.
int64_t __cajeta_file_seek(int32_t fd, int64_t offset, int32_t whence) {
    if (fd < 0) return -1;
    int w = SEEK_SET;
    if (whence == 1) w = SEEK_CUR;
    else if (whence == 2) w = SEEK_END;
    off_t r = lseek(fd, (off_t) offset, w);
    return (int64_t) r;
}

int64_t __cajeta_file_size_of(int32_t fd) {
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (int64_t) st.st_size;
}

int32_t __cajeta_file_truncate(int32_t fd, int64_t size) {
    if (fd < 0 || size < 0) return -1;
    return ftruncate(fd, (off_t) size) == 0 ? 0 : -1;
}

int32_t __cajeta_file_sync(int32_t fd) {
    if (fd < 0) return -1;
    return fsync(fd) == 0 ? 0 : -1;
}

// flock + LOCK_EX / LOCK_NB / LOCK_UN — POSIX file-locking via
// <sys/file.h>. MinGW-w64 doesn't ship sys/file.h with flock; map
// to Win32 LockFileEx / UnlockFileEx instead. _get_osfhandle turns
// a CRT fd into a Win32 HANDLE.
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

// Streaming flush. No user-space buffering today (FileWriter writes
// straight through), so this is a no-op stub. When the 8 KiB
// internal buffer lands (Phase A.2), this drains it via writev().
int32_t __cajeta_file_flush(int32_t fd) {
    (void) fd;
    return 0;
}

// Close the fd. Idempotent at the cajeta-call level: the cajeta-side
// `close()` is idempotent because it sets `this.fd = -1` after the
// first call. Passing -1 here is a no-op.
void __cajeta_file_close(int32_t fd) {
    if (fd < 0) return;
    close(fd);
}

// ===========================================================================
