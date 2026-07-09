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
    // diagnostic-exceptions U3: semantic line-info frames snapshotted from the
    // shadow line-stack at throw time (innermost-first), or NULL when line-info
    // was off. Resolved by getStackTrace into StackFrame type/method/file/line.
    CajetaShadowFrame* shadow;
    int shadow_count;
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
    // U3: snapshot the shadow line-stack (semantic frames) alongside the raw
    // addresses. Present only when codegen emitted line-info (enter/mark/leave).
    e->shadow = NULL;
    e->shadow_count = 0;
    {
        int32_t sc = __cajeta_shadow_get_top();
        if (sc > CAJETA_SHADOW_MAX) sc = CAJETA_SHADOW_MAX;
        if (sc > 0) {
            CajetaShadowFrame* snap =
                (CajetaShadowFrame*) malloc((size_t) sc * sizeof(CajetaShadowFrame));
            if (snap) {
                e->shadow_count = __cajeta_shadow_snapshot(snap, sc);
                e->shadow = snap;
            }
        }
    }
    pthread_mutex_lock(&__cajeta_trace_mutex);
    // Dedup: drop any prior entry for this same throwable address so a reused
    // address can't surface a stale trace.
    for (struct cajeta_trace_entry** pp = &__cajeta_trace_table; *pp; ) {
        if ((*pp)->throwable == throwable) {
            struct cajeta_trace_entry* dead = *pp;
            *pp = dead->next;
            free(dead->frames);
            free(dead->shadow);
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
            free(dead->shadow);
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
// Basename of a source path (last component after '/' or '\\').
static const char* cajeta_basename(const char* p) {
    const char* base = p ? p : "";
    if (p) for (const char* q = p; *q; q++) if (*q == '/' || *q == '\\') base = q + 1;
    return base;
}

// Print `e`'s frames to `out`. Caller holds __cajeta_trace_mutex; `e` may be NULL
// (no trace recorded), in which case nothing is printed.
static void cajeta_print_frames_locked(struct cajeta_trace_entry* e, FILE* out) {
    if (!e) return;
    // U3: prefer the semantic shadow frames — Type.method(File.cajeta:NN).
    if (e->shadow && e->shadow_count > 0) {
        for (int i = 0; i < e->shadow_count; i++) {
            const CajetaFrameDesc* d = e->shadow[i].desc;
            const char* t = (d && d->typeName)   ? d->typeName   : "?";
            const char* m = (d && d->methodName) ? d->methodName : "?";
            const char* f = (d && d->fileName)   ? d->fileName   : "?";
            fprintf(out, "  at %s.%s(%s:%d)\n", t, m,
                    cajeta_basename(f), e->shadow[i].line);
        }
        return;
    }
    // Fallback: raw addresses symbolized by the C library.
    char** syms = backtrace_symbols(e->frames, e->frame_count);
    if (syms) {
        for (int i = 0; i < e->frame_count; i++) fprintf(out, "  %s\n", syms[i]);
        free(syms);
    }
}

void __cajeta_print_trace(void* throwable, int32_t fd) {
    FILE* out = (fd == 1) ? stdout : stderr;
    pthread_mutex_lock(&__cajeta_trace_mutex);
    struct cajeta_trace_entry* e = __cajeta_trace_table;
    while (e && e->throwable != throwable) e = e->next;
    cajeta_print_frames_locked(e, out);
    fflush(out);
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

// Print ONE throwable's header (its message, optionally prefixed "Caused by: ")
// followed by its captured frames, to fd. Powers Throwable/Exception
// .printStackTrace(): the Cajeta side calls this per cause-chain link and walks
// getCause() polymorphically. Message extraction mirrors __cajeta_emit_uncaught
// (Throwable.message@8 -> String -> int8[] payload@8 / byteLength@16), fully
// null/low-address guarded so a legacy int-throw can't fault.
void __cajeta_print_trace_one(void* throwable, int32_t fd, int32_t caused_by) {
    FILE* out = (fd == 1) ? stdout : stderr;
    const char* mbytes = NULL;
    int mlen = 0;
    if (throwable && (uintptr_t) throwable >= 4096) {
        void* strObj = ((void**) throwable)[1];
        if (strObj && (uintptr_t) strObj >= 4096) {
            void* bytesArr = ((void**) strObj)[1];
            int32_t blen = *(int32_t*) ((char*) strObj + 16);
            if (bytesArr && (uintptr_t) bytesArr >= 4096 && blen > 0) {
                mbytes = (const char*) bytesArr + 8;
                mlen = blen;
            }
        }
    }
    if (caused_by) fprintf(out, "Caused by: ");
    if (mbytes) fprintf(out, "%.*s\n", mlen, mbytes);
    else fprintf(out, "(no message)\n");
    pthread_mutex_lock(&__cajeta_trace_mutex);
    struct cajeta_trace_entry* e = __cajeta_trace_table;
    while (e && e->throwable != throwable) e = e->next;
    cajeta_print_frames_locked(e, out);
    pthread_mutex_unlock(&__cajeta_trace_mutex);
    fflush(out);
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
// diagnostic-exceptions Unit 1 (1.2.3): under --diag-format=json an uncaught
// throw emits one NDJSON diagnostic line instead of free text. Definitions live
// with the other diagnostic natives further down; forward-declared here.
static int  __cajeta_diag_json_enabled(void);
static void __cajeta_emit_uncaught_json(void* value);
static int  cajeta_json_escape(void* strObj, char* out, int outcap);

static void __cajeta_emit_uncaught(void* value, int is_unrec) {
    if (__cajeta_diag_json_enabled()) {
        __cajeta_emit_uncaught_json(value);
        return;
    }
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
    // U3: the unwound frames never ran __cajeta_line_leave, so restore the
    // shadow line-stack depth to the catching try-frame's watermark before we
    // resume in its catch block. Snapshot already taken in __cajeta_trace_record.
    __cajeta_shadow_set_top((*excTop)->shadow_watermark);
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

// --- diagnostic-exceptions Unit 1: canonical type name + JSON escaping --------
//
// Throwable.toJson() builds the NDJSON diagnostic in Cajeta; these two natives
// supply the pieces Cajeta can't reach: the canonical RTTI type name (the
// default diagnostic `code`) and JSON-string escaping (mirrors the compiler's
// jsonEscape in Diagnostics.cpp so runtime + compile-time diagnostics agree).
// Both use the length-then-fill idiom (see cajeta_str_into above).

// diagnostic-exceptions Unit 2 (2.1.2): the diagnostic `code` is the throwable
// type's @DiagnosticCode("...") value when present, else its canonical type
// name. The annotation name is retained in the class RTTI (no retention filter),
// so it is readable at runtime with no debug info.
static const char* cajeta_diag_annotation_code(void* obj) {
    if (!obj || (uintptr_t) obj < 4096) return NULL;
    void* r = cajeta_rtti_from_obj(obj);
    if (!r) return NULL;
    CajetaRtti* rt = (CajetaRtti*) r;
    for (int i = 0; i < rt->classAnnotationCount; i++) {
        const CajetaAnnotationDesc* a = &rt->classAnnotations[i];
        if (!a->name) continue;
        size_t L = strlen(a->name);
        const size_t BL = 14;  // strlen("DiagnosticCode")
        int isDC = (L == BL && !strcmp(a->name, "DiagnosticCode")) ||
                   (L > BL && a->name[L - BL - 1] == '.'
                           && !strcmp(a->name + L - BL, "DiagnosticCode"));
        if (!isDC) continue;
        if (a->argCount > 0 && a->args && a->args[0].strVal && a->args[0].strVal[0])
            return a->args[0].strVal;
    }
    return NULL;
}
static const char* cajeta_diag_code(void* obj) {
    const char* c = cajeta_diag_annotation_code(obj);
    if (c) return c;
    if (obj && (uintptr_t) obj >= 4096) {
        void* r = cajeta_rtti_from_obj(obj);
        if (r) return ((CajetaRtti*) r)->typeName;
    }
    return NULL;
}
int32_t __cajeta_diag_code_len(void* obj) {
    const char* c = cajeta_diag_code(obj);
    return c ? (int32_t) strlen(c) : 0;
}
void __cajeta_diag_code_into(void* obj, void* out) {
    const char* c = cajeta_diag_code(obj);
    cajeta_str_into(out, c ? c : "", c ? (int) strlen(c) : 0);
}

int32_t __cajeta_type_name_len(void* obj) {
    if (!obj || (uintptr_t) obj < 4096) return 0;
    void* r = cajeta_rtti_from_obj(obj);
    const char* n = r ? ((CajetaRtti*) r)->typeName : NULL;
    return n ? (int32_t) strlen(n) : 0;
}
void __cajeta_type_name_into(void* obj, void* out) {
    const char* n = NULL;
    if (obj && (uintptr_t) obj >= 4096) {
        void* r = cajeta_rtti_from_obj(obj);
        if (r) n = ((CajetaRtti*) r)->typeName;
    }
    cajeta_str_into(out, n ? n : "", n ? (int) strlen(n) : 0);
}

// Escape a cajeta.lang.String's UTF-8 bytes for embedding inside a JSON string
// literal. `strObj` layout: { vtable@0, int8[] bytes@8, int32 byteLength@16 };
// int8[] is { int64 count@0, payload@8 }. out == NULL just counts.
static int cajeta_json_escape(void* strObj, char* out, int outcap) {
    const char* src = NULL;
    int n = 0;
    if (strObj && (uintptr_t) strObj >= 4096) {
        void* bytesArr = ((void**) strObj)[1];
        int32_t blen = *(int32_t*) ((char*) strObj + 16);
        if (bytesArr && (uintptr_t) bytesArr >= 4096 && blen > 0) {
            src = (const char*) bytesArr + 8;
            n = blen;
        }
    }
    int w = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char) src[i];
        char ubuf[8];
        const char* esc;
        int elen;
        switch (c) {
            case '"':  esc = "\\\""; elen = 2; break;
            case '\\': esc = "\\\\"; elen = 2; break;
            case '\n': esc = "\\n";  elen = 2; break;
            case '\r': esc = "\\r";  elen = 2; break;
            case '\t': esc = "\\t";  elen = 2; break;
            case '\b': esc = "\\b";  elen = 2; break;
            case '\f': esc = "\\f";  elen = 2; break;
            default:
                if (c < 0x20) {
                    snprintf(ubuf, sizeof ubuf, "\\u%04x", c);
                    esc = ubuf;
                    elen = 6;
                } else {
                    if (out && w < outcap) out[w] = (char) c;
                    w++;
                    continue;
                }
        }
        for (int k = 0; k < elen; k++) {
            if (out && w < outcap) out[w] = esc[k];
            w++;
        }
    }
    return w;
}
int32_t __cajeta_json_escape_len(void* strObj) {
    return (int32_t) cajeta_json_escape(strObj, NULL, 0);
}
void __cajeta_json_escape_into(void* strObj, void* out) {
    if (!out) return;
    int64_t cap = *((int64_t*) out);
    cajeta_json_escape(strObj, (char*) out + 8, (int) cap);
}

// --- diagnostic-exceptions Unit 2: reflection-serialized `fields` -------------
//
// Serialize a throwable's own declared instance fields (excluding the built-in
// message/cause slots) as a JSON `,"fields":{...}` fragment, driven by the RTTI
// property table (no per-type code). Primitives → JSON numbers/booleans; String
// references → escaped strings; other references are skipped. Reuses
// cajeta_return_kind + the CAJETA_RK_* kinds (cajeta_rt_inject.c). out == NULL
// counts; returns bytes written (a leading-comma fragment, or 0 when empty).
static int cajeta_diag_fields(void* obj, char* out, int outcap) {
    if (!obj || (uintptr_t) obj < 4096) return 0;
    void* r = cajeta_rtti_from_obj(obj);
    if (!r) return 0;
    CajetaRtti* rt = (CajetaRtti*) r;
    int w = 0;
    int emitted = 0;
#define CDF_PUT(s, n)  do { const char* _s=(s); int _n=(n); for (int _k=0;_k<_n;_k++){ if (out && w<outcap) out[w]=_s[_k]; w++; } } while (0)
#define CDF_PUTC(ch)   do { if (out && w<outcap) out[w]=(char)(ch); w++; } while (0)
    for (int i = 0; i < rt->propertyCount; i++) {
        const CajetaFieldDesc* f = &rt->properties[i];
        if (!f->name || f->byteOffset < 0) continue;   // unnamed or static
        if (!strcmp(f->name, "message") || !strcmp(f->name, "cause")) continue;
        char* slot = (char*) obj + f->byteOffset;
        int kind = cajeta_return_kind(f->type);
        char vbuf[64];
        int vlen = 0;
        const char* vp = NULL;    // number/literal bytes to copy verbatim
        void* strRef = NULL;      // when set, emit as an escaped JSON string
        switch (kind) {
            case CAJETA_RK_BOOLEAN: { int b = *(int8_t*) slot ? 1 : 0;
                                      vp = b ? "true" : "false"; vlen = b ? 4 : 5; break; }
            case CAJETA_RK_INT8:    vlen = snprintf(vbuf, sizeof vbuf, "%d",   (int) *(int8_t*)  slot); vp = vbuf; break;
            case CAJETA_RK_INT16:   vlen = snprintf(vbuf, sizeof vbuf, "%d",   (int) *(int16_t*) slot); vp = vbuf; break;
            case CAJETA_RK_INT32:   vlen = snprintf(vbuf, sizeof vbuf, "%d",         *(int32_t*) slot); vp = vbuf; break;
            case CAJETA_RK_INT64:   vlen = snprintf(vbuf, sizeof vbuf, "%lld", (long long) *(int64_t*) slot); vp = vbuf; break;
            case CAJETA_RK_UINT8:   vlen = snprintf(vbuf, sizeof vbuf, "%u",   (unsigned) *(uint8_t*)  slot); vp = vbuf; break;
            case CAJETA_RK_UINT16:  vlen = snprintf(vbuf, sizeof vbuf, "%u",   (unsigned) *(uint16_t*) slot); vp = vbuf; break;
            case CAJETA_RK_UINT32:  vlen = snprintf(vbuf, sizeof vbuf, "%u",         *(uint32_t*) slot); vp = vbuf; break;
            case CAJETA_RK_UINT64:  vlen = snprintf(vbuf, sizeof vbuf, "%llu", (unsigned long long) *(uint64_t*) slot); vp = vbuf; break;
            case CAJETA_RK_CHAR:    vlen = snprintf(vbuf, sizeof vbuf, "%d",         *(int32_t*) slot); vp = vbuf; break;
            case CAJETA_RK_FLOAT32: vlen = snprintf(vbuf, sizeof vbuf, "%g",  (double) *(float*)  slot); vp = vbuf; break;
            case CAJETA_RK_FLOAT64: vlen = snprintf(vbuf, sizeof vbuf, "%g",          *(double*) slot); vp = vbuf; break;
            case CAJETA_RK_REFERENCE: {
                void* ref = *(void**) slot;
                if (ref && (uintptr_t) ref >= 4096) {
                    void* rr = cajeta_rtti_from_obj(ref);
                    const char* tn = rr ? ((CajetaRtti*) rr)->typeName : NULL;
                    if (tn && !strcmp(tn, "cajeta.lang.String")) strRef = ref;
                }
                if (!strRef) continue;   // non-String references not serialized here
                break;
            }
            default: continue;           // RK_OTHER / RK_VOID
        }
        if (vlen < 0) vlen = 0;
        if (!emitted) { CDF_PUT(",\"fields\":{", 11); emitted = 1; }
        else          { CDF_PUTC(','); }
        CDF_PUTC('"'); CDF_PUT(f->name, (int) strlen(f->name)); CDF_PUTC('"'); CDF_PUTC(':');
        if (strRef) {
            CDF_PUTC('"');
            w += cajeta_json_escape(strRef, out ? out + w : NULL, out ? outcap - w : 0);
            CDF_PUTC('"');
        } else {
            CDF_PUT(vp, vlen);
        }
    }
    if (emitted) CDF_PUTC('}');
#undef CDF_PUT
#undef CDF_PUTC
    return w;
}
int32_t __cajeta_diag_fields_len(void* obj) {
    return (int32_t) cajeta_diag_fields(obj, NULL, 0);
}
void __cajeta_diag_fields_into(void* obj, void* out) {
    if (!out) return;
    int64_t cap = *((int64_t*) out);
    cajeta_diag_fields(obj, (char*) out + 8, (int) cap);
}

// --- diagnostic-exceptions Unit 3: semantic-frame accessors ------------------
//
// getStackTrace() reads the shadow snapshot recorded at throw time: a count,
// then per frame a #FrameDesc pointer + line. The desc string readers resolve
// type/method/file (length-then-fill); the role derives from the type package.

// Number of semantic (line-info) frames for `throwable`, 0 if none captured.
int32_t __cajeta_trace_shadow_count(void* throwable) {
    if (!throwable) return 0;
    pthread_mutex_lock(&__cajeta_trace_mutex);
    struct cajeta_trace_entry* e = __cajeta_trace_table;
    while (e && e->throwable != throwable) e = e->next;
    int32_t c = e ? e->shadow_count : 0;
    pthread_mutex_unlock(&__cajeta_trace_mutex);
    return c;
}
// The #FrameDesc handle for semantic frame `i` (innermost = 0), or 0. An opaque
// int64 (CajetaFrameDesc*) so the Cajeta @Native ABI stays pure i64/i32.
int64_t __cajeta_trace_shadow_desc(void* throwable, int32_t i) {
    if (!throwable || i < 0) return 0;
    pthread_mutex_lock(&__cajeta_trace_mutex);
    struct cajeta_trace_entry* e = __cajeta_trace_table;
    while (e && e->throwable != throwable) e = e->next;
    int64_t d = (e && i < e->shadow_count && e->shadow)
                  ? (int64_t) (uintptr_t) e->shadow[i].desc : 0;
    pthread_mutex_unlock(&__cajeta_trace_mutex);
    return d;
}
// The source line for semantic frame `i`, or 0.
int32_t __cajeta_trace_shadow_line(void* throwable, int32_t i) {
    if (!throwable || i < 0) return 0;
    pthread_mutex_lock(&__cajeta_trace_mutex);
    struct cajeta_trace_entry* e = __cajeta_trace_table;
    while (e && e->throwable != throwable) e = e->next;
    int32_t ln = (e && i < e->shadow_count && e->shadow) ? e->shadow[i].line : 0;
    pthread_mutex_unlock(&__cajeta_trace_mutex);
    return ln;
}

int32_t __cajeta_desc_type_len(int64_t desc) {
    const CajetaFrameDesc* d = (const CajetaFrameDesc*) (uintptr_t) desc;
    const char* s = d ? d->typeName : NULL;
    return s ? (int32_t) strlen(s) : 0;
}
void __cajeta_desc_type_into(int64_t desc, void* out) {
    const CajetaFrameDesc* d = (const CajetaFrameDesc*) (uintptr_t) desc;
    const char* s = d ? d->typeName : NULL;
    cajeta_str_into(out, s ? s : "", s ? (int) strlen(s) : 0);
}
int32_t __cajeta_desc_method_len(int64_t desc) {
    const CajetaFrameDesc* d = (const CajetaFrameDesc*) (uintptr_t) desc;
    const char* s = d ? d->methodName : NULL;
    return s ? (int32_t) strlen(s) : 0;
}
void __cajeta_desc_method_into(int64_t desc, void* out) {
    const CajetaFrameDesc* d = (const CajetaFrameDesc*) (uintptr_t) desc;
    const char* s = d ? d->methodName : NULL;
    cajeta_str_into(out, s ? s : "", s ? (int) strlen(s) : 0);
}
int32_t __cajeta_desc_file_len(int64_t desc) {
    const CajetaFrameDesc* d = (const CajetaFrameDesc*) (uintptr_t) desc;
    const char* s = d ? d->fileName : NULL;
    return s ? (int32_t) strlen(s) : 0;
}
void __cajeta_desc_file_into(int64_t desc, void* out) {
    const CajetaFrameDesc* d = (const CajetaFrameDesc*) (uintptr_t) desc;
    const char* s = d ? d->fileName : NULL;
    cajeta_str_into(out, s ? s : "", s ? (int) strlen(s) : 0);
}
// FrameRole ordinal: 0 User, 1 Stdlib (cajeta.* package), 2 Runtime (no desc).
int32_t __cajeta_desc_role(int64_t desc) {
    const CajetaFrameDesc* d = (const CajetaFrameDesc*) (uintptr_t) desc;
    if (!d) return 2;
    const char* tn = d->typeName;
    if (tn && strncmp(tn, "cajeta.", 7) == 0) return 1;
    return 0;
}

// Diagnostic output format for the uncaught-throw path. -1 = uninitialized (read
// from the CAJETA_DIAG_FORMAT env once); 0 = text; 1 = json. The JIT host sets it
// explicitly from --diag-format; an AOT exe picks it up from the environment.
static int __cajeta_diag_format_json = -1;

void __cajeta_set_diag_format_json(int enabled) {
    __cajeta_diag_format_json = enabled ? 1 : 0;
}
static int __cajeta_diag_json_enabled(void) {
    if (__cajeta_diag_format_json < 0) {
        const char* v = getenv("CAJETA_DIAG_FORMAT");
        __cajeta_diag_format_json = (v && strcmp(v, "json") == 0) ? 1 : 0;
    }
    return __cajeta_diag_format_json;
}

// Write one NDJSON diagnostic line for an uncaught throw (severity/code/message/
// frames), a superset of the compiler's --diag-format=json shape. Reuses the
// canonical type name (code), JSON escaping, and the throw-site trace table.
static void __cajeta_emit_uncaught_json(void* value) {
    void* strObj = NULL;
    if (value && (uintptr_t) value >= 4096) strObj = ((void**) value)[1];
    int esclen = cajeta_json_escape(strObj, NULL, 0);

    const char* tn = cajeta_diag_code(value);
    if (!tn) tn = "cajeta.error.Throwable";
    size_t tnlen = strlen(tn);

    // Copy the frame info out under the trace lock: semantic shadow frames when
    // present (type/method/file/line), else raw return addresses.
    uintptr_t addrs[CAJETA_TRACE_MAX_FRAMES];
    int addr_count = 0;
    CajetaShadowFrame sh[CAJETA_TRACE_MAX_FRAMES];
    int sh_count = 0;
    size_t sh_strbytes = 0;
    pthread_mutex_lock(&__cajeta_trace_mutex);
    struct cajeta_trace_entry* e = __cajeta_trace_table;
    while (e && e->throwable != value) e = e->next;
    if (e && e->shadow && e->shadow_count > 0) {
        sh_count = e->shadow_count;
        if (sh_count > CAJETA_TRACE_MAX_FRAMES) sh_count = CAJETA_TRACE_MAX_FRAMES;
        for (int i = 0; i < sh_count; i++) {
            sh[i] = e->shadow[i];
            const CajetaFrameDesc* d = sh[i].desc;
            if (d) {
                if (d->typeName)   sh_strbytes += strlen(d->typeName);
                if (d->methodName) sh_strbytes += strlen(d->methodName);
                if (d->fileName)   sh_strbytes += strlen(d->fileName);
            }
        }
    } else if (e) {
        addr_count = e->frame_count;
        if (addr_count > CAJETA_TRACE_MAX_FRAMES) addr_count = CAJETA_TRACE_MAX_FRAMES;
        for (int i = 0; i < addr_count; i++) addrs[i] = (uintptr_t) e->frames[i];
    }
    pthread_mutex_unlock(&__cajeta_trace_mutex);

    size_t cap = 256 + (size_t) esclen + tnlen
               + (size_t) addr_count * 48
               + (size_t) sh_count * 96 + sh_strbytes;
    char* buf = (char*) malloc(cap);
    if (!buf) return;
    size_t o = 0;
    int w = snprintf(buf + o, cap - o,
                     "{\"severity\":\"error\",\"code\":\"%.*s\",\"message\":\"",
                     (int) tnlen, tn);
    if (w > 0) o += (size_t) w;
    if (o >= cap) o = cap - 1;
    o += (size_t) cajeta_json_escape(strObj, buf + o, (int) (cap - o));
    if (o >= cap) o = cap - 1;
    w = snprintf(buf + o, cap - o, "\",\"frames\":[");
    if (w > 0) o += (size_t) w;
    if (sh_count > 0) {
        for (int i = 0; i < sh_count && o < cap - 1; i++) {
            const CajetaFrameDesc* d = sh[i].desc;
            const char* t = (d && d->typeName)   ? d->typeName   : "";
            const char* m = (d && d->methodName) ? d->methodName : "";
            const char* f = (d && d->fileName)   ? cajeta_basename(d->fileName) : "";
            const char* role = (d && d->typeName
                                && strncmp(d->typeName, "cajeta.", 7) == 0)
                                   ? "Stdlib" : "User";
            w = snprintf(buf + o, cap - o,
                "%s{\"declaringType\":\"%s\",\"method\":\"%s\",\"file\":\"%s\","
                "\"line\":%d,\"role\":\"%s\"}",
                i ? "," : "", t, m, f, sh[i].line, role);
            if (w > 0) o += (size_t) w;
            if (o >= cap) { o = cap - 1; break; }
        }
    } else {
        for (int i = 0; i < addr_count && o < cap - 1; i++) {
            w = snprintf(buf + o, cap - o, "%s{\"nativeAddress\":%llu}",
                         i ? "," : "", (unsigned long long) addrs[i]);
            if (w > 0) o += (size_t) w;
            if (o >= cap) { o = cap - 1; break; }
        }
    }
    w = snprintf(buf + o, cap - o, "]}\n");
    if (w > 0) o += (size_t) w;
    if (o > cap) o = cap;
    (void) write(2, buf, o);
    free(buf);
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
