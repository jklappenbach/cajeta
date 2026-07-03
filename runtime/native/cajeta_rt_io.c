// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// --- R5/Error-model #203: stack-trace capture ---------------------------
//
// At every throw site, walk the native call stack via backtrace() and
// store the return-address array in a side table keyed by the throwable
// pointer. Auto-printed on uncaught throws (when the runtime aborts) and
// retrievable via __cajeta_get_trace / __cajeta_print_trace for user-
// invoked introspection. Side-table avoids the Throwable struct-layout
// problem (subclasses don't carry parent fields in their memory image
// today — see follow-up #208 — so we can't reliably stash the trace as
// a field on Throwable). When that's fixed, this can migrate to a real
// field with no API change at the Cajeta level.

struct cajeta_trace_entry {
    void* throwable;
    void** frames;
    int frame_count;
    struct cajeta_trace_entry* next;
};

static struct cajeta_trace_entry* __cajeta_trace_table = NULL;
static int __cajeta_trace_count = 0;
static pthread_mutex_t __cajeta_trace_mutex = PTHREAD_MUTEX_INITIALIZER;

#define CAJETA_TRACE_MAX_FRAMES 64
// There is no "throwable caught/dropped" hook that frees a trace entry yet, so a
// throw/catch loop would leak one node + frames array per throw. Bound the table:
// dedup same-throwable on record, and evict the oldest past this cap.
#define CAJETA_TRACE_TABLE_CAP 256

// --stack-trace-capture flag (CompilerModes.md § --stack-trace-capture).
// Defaults on so existing tests + the default ergonomic of "see the
// stack on uncaught throw" both keep working. JIT init flips it via
// Options.stackTraceCaptureEnabled. When off, __cajeta_trace_record is
// a fast no-op — the throwable still surfaces (message + error id) but
// the trace dump on uncaught throws is empty.
static int __cajeta_stack_trace_capture_enabled = 1;

void __cajeta_set_stack_trace_capture(int enabled) {
    __cajeta_stack_trace_capture_enabled = enabled ? 1 : 0;
}

int __cajeta_get_stack_trace_capture(void) {
    return __cajeta_stack_trace_capture_enabled;
}

static void __cajeta_trace_record(void* throwable) {
    if (!throwable) return;
    if (!__cajeta_stack_trace_capture_enabled) return;
    // Skip trace capture inside a fiber. backtrace(3) walks the stack via
    // frame pointers / DWARF; on a makecontext-allocated fiber stack the
    // unwinder reaches the makecontext boundary and SIGSEGVs trying to walk
    // past it. Re-enable when fiber-aware unwinding lands.
    if (__cajeta_current_fiber) return;
    void* buf[CAJETA_TRACE_MAX_FRAMES];
    int n = backtrace(buf, CAJETA_TRACE_MAX_FRAMES);
    if (n <= 0) return;
    void** frames = (void**) malloc((size_t) n * sizeof(void*));
    if (!frames) return;
    memcpy(frames, buf, (size_t) n * sizeof(void*));
    struct cajeta_trace_entry* e =
        (struct cajeta_trace_entry*) malloc(sizeof(*e));
    if (!e) { free(frames); return; }
    e->throwable = throwable;
    e->frames = frames;
    e->frame_count = n;
    pthread_mutex_lock(&__cajeta_trace_mutex);
    // Dedup: drop any prior entry for this same throwable address so a reused
    // address can't surface a stale trace.
    for (struct cajeta_trace_entry** pp = &__cajeta_trace_table; *pp; ) {
        if ((*pp)->throwable == throwable) {
            struct cajeta_trace_entry* dead = *pp;
            *pp = dead->next;
            free(dead->frames);
            free(dead);
            __cajeta_trace_count--;
        } else {
            pp = &(*pp)->next;
        }
    }
    // Cap: evict the oldest (tail) entry when at capacity — bounds the leak.
    if (__cajeta_trace_count >= CAJETA_TRACE_TABLE_CAP) {
        struct cajeta_trace_entry** tail = &__cajeta_trace_table;
        while (*tail && (*tail)->next) tail = &(*tail)->next;
        if (*tail) {
            struct cajeta_trace_entry* dead = *tail;
            *tail = NULL;
            free(dead->frames);
            free(dead);
            __cajeta_trace_count--;
        }
    }
    e->next = __cajeta_trace_table;
    __cajeta_trace_table = e;
    __cajeta_trace_count++;
    pthread_mutex_unlock(&__cajeta_trace_mutex);
}

// Print the trace for `throwable` to fd (1=stdout, 2=stderr). No-op if
// no trace was recorded for this throwable.
void __cajeta_print_trace(void* throwable, int32_t fd) {
    pthread_mutex_lock(&__cajeta_trace_mutex);
    struct cajeta_trace_entry* e = __cajeta_trace_table;
    while (e && e->throwable != throwable) e = e->next;
    if (!e) { pthread_mutex_unlock(&__cajeta_trace_mutex); return; }
    char** syms = backtrace_symbols(e->frames, e->frame_count);
    if (syms) {
        FILE* out = (fd == 1) ? stdout : stderr;
        for (int i = 0; i < e->frame_count; i++) {
            fprintf(out, "  %s\n", syms[i]);
        }
        fflush(out);
        free(syms);
    }
    pthread_mutex_unlock(&__cajeta_trace_mutex);
}

// Copy up to `max` captured frame return-addresses for `throwable` into the
// Cajeta int64[] `out_arr` (layout { i64 count@0, i64 payload@8 }), returning
// the number written. 0 when no trace was recorded (capture off / fiber /
// Windows / already evicted). Powers Throwable.getStackTrace().
int32_t __cajeta_get_trace(void* throwable, void* out_arr, int32_t max) {
    if (!throwable || !out_arr || max <= 0) return 0;
    int64_t cap = *((int64_t*) out_arr);               // element capacity
    int64_t* out = (int64_t*) ((char*) out_arr + 8);   // payload
    pthread_mutex_lock(&__cajeta_trace_mutex);
    struct cajeta_trace_entry* e = __cajeta_trace_table;
    while (e && e->throwable != throwable) e = e->next;
    if (!e) { pthread_mutex_unlock(&__cajeta_trace_mutex); return 0; }
    int n = e->frame_count;
    if (n > max) n = max;
    if (n > cap) n = (int) cap;
    for (int i = 0; i < n; i++)
        out[i] = (int64_t) (uintptr_t) e->frames[i];
    pthread_mutex_unlock(&__cajeta_trace_mutex);
    return n;
}

// Helper for the uncaught-throw path: print the throwable's message
// (if any) and stack trace to stderr. Mirrors Java/Python's "Exception
// in thread main: ... \n Traceback: ..." shape. Throwable layout is
// { vtable, message, ... } so the message field is at offset 8 in
// any class derived from Throwable — see ErrorModel.md § hierarchy.
// If the value isn't a Throwable instance (e.g. legacy int throw via
// IntToPtr), the message read may yield garbage; we print the raw
// pointer in that case as a hex fallback. The trace is recorded at
// throw time regardless of whether the throwable carries a message.
static void __cajeta_emit_uncaught(void* value, int is_unrec) {
    const char* kind = is_unrec ? "unrecoverable" : "uncaught";
    // Throwable.message (slot 1) is a Cajeta String OBJECT, not a C string:
    //   Throwable { vtable@0, String message@8 }
    //   String    { vtable@0, int8[] bytes@8, int32 byteLength@16, ... }
    //   int8[]    { i64 count@0, payload@8 }
    // Extract the UTF-8 payload + byteLength and print bounded with %.*s. Every
    // hop is null/low-address guarded (legacy int throws via IntToPtr, or a
    // null/empty message); on any failure fall back to the bare hex value.
    const char* mbytes = NULL;
    int mlen = 0;
    if (value && (uintptr_t) value >= 4096) {
        void* strObj = ((void**) value)[1];                 // Throwable.message
        if (strObj && (uintptr_t) strObj >= 4096) {
            void* bytesArr = ((void**) strObj)[1];           // String.bytes (int8[])
            int32_t blen = *(int32_t*) ((char*) strObj + 16);  // String.byteLength
            if (bytesArr && (uintptr_t) bytesArr >= 4096 && blen > 0) {
                mbytes = (const char*) bytesArr + 8;         // skip the i64 count header
                mlen = blen;
            }
        }
    }
    // write(2), not fprintf(stderr): the caller abort()s (unrecoverable) or
    // exit()s, and abort() doesn't flush stdio. On Windows stderr is block-
    // buffered when piped (e.g. under a gtest death test), so an fprintf'd
    // message never reaches the fd. Format into a stack buffer, then write the
    // raw bytes to fd 2 directly.
    char buf[1024];
    int n;
    if (mbytes) {
        n = snprintf(buf, sizeof(buf), "cajeta: %s exception: %.*s\n",
                     kind, mlen, mbytes);
    } else {
        n = snprintf(buf, sizeof(buf), "cajeta: %s exception (value=%p)\n",
                     kind, value);
    }
    if (n > 0) {
        if (n > (int) sizeof(buf)) n = (int) sizeof(buf);
        (void) write(2, buf, (size_t) n);
    }
    __cajeta_print_trace(value, 2);
}

__attribute__((noreturn))
void __cajeta_throw(void* value) {
    __cajeta_trace_record(value);
    // CP6f-3: exception breakpoint. Notify the debugger BEFORE unwinding drops
    // or longjmping, so the throwing frame chain (and its locals) are still
    // live to inspect while parked. No-op when no handler is installed.
    {
        cajeta_dbg_exception_fn xh = __cajeta_dbg_exception_handler;
        if (xh) xh(value, __cajeta_dbg_current_fiber_id(),
                   *__cajeta_dbg_top_ptr());
    }
    struct cajeta_exception_frame** excTop = __cajeta_exc_top_ptr();
    if (!*excTop) {
        int is_unrec = __cajeta_is_unrecoverable(value);
        __cajeta_emit_uncaught(value, is_unrec);
        if (is_unrec) {
            // Alarm semantics — abort produces a SIGABRT, dump-friendly.
            abort();
        }
        // Recoverable that escaped every handler. Exit cleanly with a
        // nonzero code. The runtime's stderr emission above is the user-
        // facing diagnostic.
        exit(1);
    }
    // Unwind drops between the current top and the catching frame's watermark.
    // Each active entry runs its drop function once; the entry itself is
    // stack-allocated in the originating frame, so we only manipulate the
    // chain pointer here.
    struct cajeta_drop_entry** dropTop = __cajeta_drop_top_ptr();
    struct cajeta_drop_entry* watermark = (*excTop)->drop_watermark;
    while (*dropTop != watermark) {
        struct cajeta_drop_entry* e = *dropTop;
        if (!e) break;  // shouldn't happen, but bail rather than loop
        if (e->active && e->drop_fn) {
            __atomic_fetch_add(&__cajeta_drop_count, 1, __ATOMIC_SEQ_CST);
            e->drop_fn(e->obj);
        }
        *dropTop = e->prev;
    }
    (*excTop)->thrown_value = value;
    longjmp((*excTop)->buf, 1);
}

void* __cajeta_get_thrown(void) {
    struct cajeta_exception_frame** top = __cajeta_exc_top_ptr();
    return *top ? (*top)->thrown_value : NULL;
}

// --- I/O helpers: print / println / log (SLF4J-style {} templating) ----------
//
// The compiler emits direct calls to these for System.{stdout,stderr,stdin}
// .{print,println,printf}(...). stream is the file descriptor: 0=stdin,
// 1=stdout, 2=stderr. Writing to stdin is unusual but supported because the
// language exposes the same surface on all three streams.

#include <unistd.h>

static void __cajeta_emit(int32_t stream, const char* s, size_t n) {
    if (!s || n == 0) return;
    // Use write() so output is unbuffered relative to the host's stdio buffers —
    // matters for tests that capture descriptor-level output.
    ssize_t r = write(stream, s, n);
    (void) r;  // best-effort; ignore short writes / EBADF
}

void __cajeta_print(int32_t stream, const char* s) {
    if (!s) return;
    __cajeta_emit(stream, s, strlen(s));
}

void __cajeta_println(int32_t stream, const char* s) {
    if (s) __cajeta_emit(stream, s, strlen(s));
    __cajeta_emit(stream, "\n", 1);
}

// Primitive overloads — the compiler picks one based on the static type of the
// argument expression. Integers are widened to i64, floats to f64. Booleans
// stringify to "true"/"false" (matching Java's PrintStream.println(boolean)).
void __cajeta_print_i64(int32_t stream, int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long) v);
    if (n > 0) __cajeta_emit(stream, buf, (size_t) n);
}
void __cajeta_println_i64(int32_t stream, int64_t v) {
    __cajeta_print_i64(stream, v);
    __cajeta_emit(stream, "\n", 1);
}

void __cajeta_print_f64(int32_t stream, double v) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%g", v);
    if (n > 0) __cajeta_emit(stream, buf, (size_t) n);
}
void __cajeta_println_f64(int32_t stream, double v) {
    __cajeta_print_f64(stream, v);
    __cajeta_emit(stream, "\n", 1);
}

void __cajeta_print_bool(int32_t stream, int32_t v) {
    if (v) __cajeta_emit(stream, "true", 4);
    else   __cajeta_emit(stream, "false", 5);
}
void __cajeta_println_bool(int32_t stream, int32_t v) {
    __cajeta_print_bool(stream, v);
    __cajeta_emit(stream, "\n", 1);
}

// --- string concatenation helpers --------------------------------------------
//
// The compiler emits these when it sees `+` with at least one String operand.
// Concatenation results and stringified primitives are heap-allocated and leak
// today — Cajeta's owner/borrower memory model isn't wired through these yet.
// Acceptable for now: typical workloads concat a bounded number of times per
// log call, not in tight loops; a future pass will thread the lifetime through
// the IR.

char* __cajeta_i64_to_str(int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long) v);
    if (n < 0) n = 0;
    char* out = (char*) malloc((size_t) n + 1);
    if (!out) return NULL;
    memcpy(out, buf, (size_t) n);
    out[n] = '\0';
    return out;
}

// Lever #1 (string-concat fast path): decimal length of an int64, no allocation.
// Lets the concat lowering size its destination buffer for an integer operand
// without first malloc'ing a stringified copy. Counts the sign for negatives;
// the unsigned magnitude is computed via a wrap-safe negate (handles INT64_MIN).
int64_t __cajeta_i64_str_len(int64_t v) {
    int64_t n = 0;
    uint64_t u;
    if (v < 0) { n = 1; u = (uint64_t) (-(v + 1)) + 1u; }
    else       { u = (uint64_t) v; }
    if (u == 0) return 1;
    while (u) { u /= 10u; n++; }
    return n;
}

// Lever #1: write the decimal text of an int64 straight into `dst` (no NUL, no
// allocation) and return the byte count. The concat lowering calls this to format
// an integer operand directly into the result String's byte storage, eliminating
// the per-concat __cajeta_i64_to_str malloc/free (60k/iter in the hashmap-string
// bench). `dst` must hold __cajeta_i64_str_len(v) bytes. Hand-rolled decimal — NOT
// snprintf: profiling showed snprintf's printf machinery (__printf_buffer/_itoa_word/
// __vsnprintf) dominating the bench; a digit loop is ~5x cheaper and locale-free.
int64_t __cajeta_i64_to_buf(int64_t v, char* dst) {
    char tmp[20];
    int i = 0;
    int neg = 0;
    uint64_t u;
    if (v < 0) { neg = 1; u = (uint64_t) (-(v + 1)) + 1u; }
    else       { u = (uint64_t) v; }
    if (u == 0) { dst[0] = '0'; return 1; }
    while (u) { tmp[i++] = (char) ('0' + (int) (u % 10u)); u /= 10u; }
    int64_t n = 0;
    if (neg) dst[n++] = '-';
    while (i > 0) { dst[n++] = tmp[--i]; }
    return n;
}

char* __cajeta_f64_to_str(double v) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%g", v);
    if (n < 0) n = 0;
    char* out = (char*) malloc((size_t) n + 1);
    if (!out) return NULL;
    memcpy(out, buf, (size_t) n);
    out[n] = '\0';
    return out;
}

// Boolean stringification returns a static literal — callers must not free it.
// All other to_str helpers return malloc'd memory; concat treats every input as
// borrowed (never frees) to keep the rule uniform.
const char* __cajeta_bool_to_str(int32_t v) {
    return v ? "true" : "false";
}

// --- wrapper-type toString() formatters (plan W6) ----------------------------
//
// `cajeta.lang` numeric/Boolean wrappers render their value into a heap
// `cajeta.lang.String` using the same length-then-fill idiom the reflection
// API uses for names (Method.getName / Field.getName): a `_len` native sizes
// the decimal/`true`/`false` text, then an `_into` native copies it into a
// caller-allocated `int8[]` whose layout is `{ int64 capacity; bytes... }`
// (the cajeta array header is the 8-byte count, data follows). No allocation
// crosses the boundary, so there's nothing to leak — unlike the `_to_str`
// concat helpers above. Signed/unsigned/float are split so a `uint64` with the
// high bit set formats as its true magnitude (%llu), not a negative %lld.
static void cajeta_str_into(void* out, const char* src, int n) {
    if (!out) return;
    if (n < 0) n = 0;
    int64_t cap = *((int64_t*) out);
    if ((int64_t) n > cap) n = (int) cap;
    if (n > 0) memcpy((char*) out + 8, src, (size_t) n);
}

int32_t __cajeta_int64_to_str_len(int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long) v);
    return n < 0 ? 0 : n;
}
void __cajeta_int64_to_str_into(int64_t v, void* out) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long) v);
    cajeta_str_into(out, buf, n);
}

int32_t __cajeta_uint64_to_str_len(uint64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long) v);
    return n < 0 ? 0 : n;
}
void __cajeta_uint64_to_str_into(uint64_t v, void* out) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long) v);
    cajeta_str_into(out, buf, n);
}

int32_t __cajeta_float64_to_str_len(double v) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%g", v);
    return n < 0 ? 0 : n;
}
void __cajeta_float64_to_str_into(double v, void* out) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%g", v);
    cajeta_str_into(out, buf, n);
}

int32_t __cajeta_bool_to_str_len(int32_t v) {
    return v ? 4 : 5;   // "true" / "false"
}
void __cajeta_bool_to_str_into(int32_t v, void* out) {
    const char* s = v ? "true" : "false";
    cajeta_str_into(out, s, (int) strlen(s));
}

// Copy `length` bytes from `data` into a freshly malloc'd null-terminated
// string. Used by struct-view field reads on `String`-typed fields: the
// inline bytes in the buffer aren't null-terminated, so we materialize an
// owned copy that's compatible with the existing String stdlib (strlen,
// strcmp, etc.). Caller takes ownership of the result.
char* __cajeta_str_view_to_owned(const char* data, int64_t length) {
    if (length < 0) length = 0;
    char* out = (char*) malloc((size_t) length + 1);
    if (!out) return NULL;
    if (data && length > 0) memcpy(out, data, (size_t) length);
    out[length] = '\0';
    return out;
}

int64_t __cajeta_str_len(const char* s) {
    return s ? (int64_t) strlen(s) : 0;
}

// Returns 1 if both strings are non-null and byte-equal, 0 otherwise. Two nulls
// are considered NOT equal to match the principle that null is not a value —
// adjust if/when null-semantics consolidate around Java's behavior.
int32_t __cajeta_str_equals(const char* a, const char* b) {
    if (!a || !b) return 0;
    return strcmp(a, b) == 0 ? 1 : 0;
}

int32_t __cajeta_str_isEmpty(const char* s) {
    return (!s || s[0] == '\0') ? 1 : 0;
}

// charAt returns the byte at `index` as int8. Out-of-range or null returns 0
// (matches a zero-default rather than throwing — exceptions in Cajeta require a
// live try-frame, which a primitive accessor shouldn't assume).
int8_t __cajeta_str_charAt(const char* s, int64_t index) {
    if (!s || index < 0) return 0;
    size_t n = strlen(s);
    if ((size_t) index >= n) return 0;
    return (int8_t) s[index];
}

int64_t __cajeta_str_indexOf(const char* s, const char* needle) {
    if (!s || !needle) return -1;
    const char* p = strstr(s, needle);
    return p ? (int64_t) (p - s) : -1;
}

int32_t __cajeta_str_startsWith(const char* s, const char* prefix) {
    if (!s || !prefix) return 0;
    size_t lp = strlen(prefix);
    return strncmp(s, prefix, lp) == 0 ? 1 : 0;
}

int32_t __cajeta_str_endsWith(const char* s, const char* suffix) {
    if (!s || !suffix) return 0;
    size_t ls = strlen(s);
    size_t lf = strlen(suffix);
    if (lf > ls) return 0;
    return memcmp(s + ls - lf, suffix, lf) == 0 ? 1 : 0;
}

int32_t __cajeta_str_contains(const char* s, const char* needle) {
    if (!s || !needle) return 0;
    return strstr(s, needle) != NULL ? 1 : 0;
}

// Returns a malloc'd ASCII-uppercased copy. Non-ASCII bytes pass through
// unchanged — multibyte/UTF-8 case folding needs a real locale-aware library
// that's outside the scope of the embedded runtime today.
char* __cajeta_str_toUpperCase(const char* s) {
    if (!s) {
        char* out = (char*) malloc(1);
        if (out) out[0] = '\0';
        return out;
    }
    size_t n = strlen(s);
    char* out = (char*) malloc(n + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) s[i];
        out[i] = (c >= 'a' && c <= 'z') ? (char) (c - 32) : (char) c;
    }
    out[n] = '\0';
    return out;
}

char* __cajeta_str_toLowerCase(const char* s) {
    if (!s) {
        char* out = (char*) malloc(1);
        if (out) out[0] = '\0';
        return out;
    }
    size_t n = strlen(s);
    char* out = (char*) malloc(n + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) s[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char) (c + 32) : (char) c;
    }
    out[n] = '\0';
    return out;
}

// Strip ASCII whitespace from both ends. Mirrors Java's String.trim, which
// trims only U+0020 and below — not the broader Character.isWhitespace set.
char* __cajeta_str_trim(const char* s) {
    if (!s) {
        char* out = (char*) malloc(1);
        if (out) out[0] = '\0';
        return out;
    }
    size_t n = strlen(s);
    size_t i = 0;
    while (i < n && (unsigned char) s[i] <= 0x20) i++;
    size_t j = n;
    while (j > i && (unsigned char) s[j - 1] <= 0x20) j--;
    size_t len = j - i;
    char* out = (char*) malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s + i, len);
    out[len] = '\0';
    return out;
}

// Replace every occurrence of `from` in `s` with `to`. Returns malloc'd.
// If `from` is empty or null, the input is returned as a fresh copy.
char* __cajeta_str_replace(const char* s, const char* from, const char* to) {
    if (!s) {
        char* out = (char*) malloc(1);
        if (out) out[0] = '\0';
        return out;
    }
    size_t slen = strlen(s);
    if (!from || from[0] == '\0') {
        char* out = (char*) malloc(slen + 1);
        if (out) memcpy(out, s, slen + 1);
        return out;
    }
    if (!to) to = "";
    size_t flen = strlen(from);
    size_t tlen = strlen(to);
    // Count occurrences to size the output buffer.
    size_t count = 0;
    const char* p = s;
    while ((p = strstr(p, from)) != NULL) { count++; p += flen; }
    size_t outLen = slen + count * (tlen > flen ? (tlen - flen) : 0)
                         - count * (flen > tlen ? (flen - tlen) : 0);
    char* out = (char*) malloc(outLen + 1);
    if (!out) return NULL;
    char* w = out;
    p = s;
    while (1) {
        const char* m = strstr(p, from);
        if (!m) { strcpy(w, p); break; }
        size_t pre = (size_t) (m - p);
        memcpy(w, p, pre);
        w += pre;
        memcpy(w, to, tlen);
        w += tlen;
        p = m + flen;
    }
    return out;
}

// --- system / time / random helpers -----------------------------------------
