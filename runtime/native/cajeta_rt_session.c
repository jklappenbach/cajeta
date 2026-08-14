// Session-binding registry (script-units spec §4).
//
// Top-level bindings of a script unit are owned by the SESSION, not by the
// synthesized entry's drop frame: codegen registers each owner here instead
// of pushing a function-local drop entry, so the value survives the entry's
// return. The host decides the session's end: `cajeta run` after the entry
// returns; the Jupyter kernel at shutdown/reset. Rebinding a name drops the
// previous occupant immediately and keeps the name's ORIGINAL position, so
// `__cajeta_session_drop_all` fires in reverse FIRST-binding order — the
// deterministic mirror of scope-exit drops (spec §4.3/§4.4).
//
// Single-threaded by contract: bindings are created and dropped on the
// session's execution thread (the same thread that owns the compiler
// front-end reuse machinery). No locking here.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* name;                 // owned copy
    void* obj;                  // the bound value (null after drop_all)
    void (*drop_fn)(void*);     // same signature drop entries use
} cajeta_session_slot;

static cajeta_session_slot* __cajeta_session_slots = 0;
static int64_t __cajeta_session_len = 0;
static int64_t __cajeta_session_cap = 0;

static cajeta_session_slot* cajeta_session_find(const char* name) {
    for (int64_t i = 0; i < __cajeta_session_len; ++i) {
        if (strcmp(__cajeta_session_slots[i].name, name) == 0) {
            return &__cajeta_session_slots[i];
        }
    }
    return 0;
}

void __cajeta_session_bind(const char* name, void* obj,
                           void (*drop_fn)(void*)) {
    cajeta_session_slot* slot = cajeta_session_find(name);
    if (slot) {
        // Rebind: drop the old occupant NOW (spec §4.3); the name keeps its
        // original position for reverse-order drop_all.
        if (slot->obj && slot->drop_fn) slot->drop_fn(slot->obj);
        slot->obj = obj;
        slot->drop_fn = drop_fn;
        return;
    }
    if (__cajeta_session_len == __cajeta_session_cap) {
        int64_t cap = __cajeta_session_cap ? __cajeta_session_cap * 2 : 8;
        cajeta_session_slot* next = (cajeta_session_slot*) realloc(
            __cajeta_session_slots, (size_t) cap * sizeof(cajeta_session_slot));
        if (!next) return;  // OOM: leak rather than corrupt
        __cajeta_session_slots = next;
        __cajeta_session_cap = cap;
    }
    cajeta_session_slot* s = &__cajeta_session_slots[__cajeta_session_len++];
    size_t n = strlen(name) + 1;
    s->name = (char*) malloc(n);
    if (s->name) memcpy(s->name, name, n);
    s->obj = obj;
    s->drop_fn = drop_fn;
}

// Drop for a BOXED primitive (see __cajeta_session_bind_value): the box is a
// plain malloc'd buffer, so releasing it is just free.
static void cajeta_session_free_box(void* p) {
    free(p);
}

// Bind a PRIMITIVE by value. A primitive top-level binding has no drop entry
// — nothing owns it, so the owner-promotion path never sees it — and its
// storage is a slot in the unit entry's frame, which is gone by the time a
// later unit reads the name. Copy the bytes into a session-owned box instead,
// and register that with the ordinary bind path so rebinding, reverse-order
// drop_all, and first-binding position all behave exactly as they do for an
// owner. `__cajeta_session_get` then returns the box, which IS the address of
// the value — the reader loads through it directly.
void __cajeta_session_bind_value(const char* name, const void* src,
                                 int64_t size) {
    if (!src || size <= 0) return;
    void* box = malloc((size_t) size);
    if (!box) return;  // OOM: leave the previous binding intact
    memcpy(box, src, (size_t) size);
    __cajeta_session_bind(name, box, cajeta_session_free_box);
}

// Ownership left the session (a `#` transfer moved the binding's title to a
// new owner): quiet the slot WITHOUT dropping — the new owner drops. The
// name keeps its position; a later rebind reoccupies the same slot.
void __cajeta_session_disarm(const char* name) {
    cajeta_session_slot* slot = cajeta_session_find(name);
    if (!slot) return;
    slot->obj = 0;
    slot->drop_fn = 0;
}

void* __cajeta_session_get(const char* name) {
    cajeta_session_slot* slot = cajeta_session_find(name);
    return slot ? slot->obj : 0;
}

int64_t __cajeta_session_count(void) {
    return __cajeta_session_len;
}

// Drop every live binding in reverse first-binding order, then reset the
// registry to empty (names freed). Safe to call repeatedly.
void __cajeta_session_drop_all(void) {
    for (int64_t i = __cajeta_session_len - 1; i >= 0; --i) {
        cajeta_session_slot* s = &__cajeta_session_slots[i];
        if (s->obj && s->drop_fn) s->drop_fn(s->obj);
        s->obj = 0;
        s->drop_fn = 0;
        free(s->name);
        s->name = 0;
    }
    __cajeta_session_len = 0;
}

// --- the unit RESULT: Out[N] (jupyter-kernel spec 4.2) --------------------
//
// A cell ending in an expression displays that expression's value. The value
// is rendered to text by CODEGEN, where the expression's type is known, and
// parked here for the host to collect once the entry returns.
//
// It rides a side channel rather than the entry's return value on purpose:
// whether a trailing expression HAS a value is only decidable after type
// resolution, long after the entry's signature is fixed. A return-typed
// result would force that decision on the synthesizer, which sees only token
// text and cannot tell `x + y;` from `xs.add(1);`.
//
// "No result" and "a result that rendered as the empty string" are different
// answers, so presence is tracked separately from the text.
static char* __cajeta_script_result_text = 0;
static int __cajeta_script_result_present = 0;

void __cajeta_script_result_clear(void) {
    free(__cajeta_script_result_text);
    __cajeta_script_result_text = 0;
    __cajeta_script_result_present = 0;
}

// Codegen hands over a C string it does not transfer ownership of (it may be
// a borrowed window into a String); we copy.
//
// A NULL is "nothing rendered" and leaves whatever is already parked — codegen
// stores a type-name placeholder before attempting a render, so a `toString`
// returning null degrades to that instead of blanking the result. An empty
// but non-null string is a real empty result and does replace it.
void __cajeta_script_result(const char* text) {
    if (!text) return;
    free(__cajeta_script_result_text);
    __cajeta_script_result_text = 0;
    __cajeta_script_result_present = 1;
    size_t n = strlen(text) + 1;
    __cajeta_script_result_text = (char*) malloc(n);
    if (__cajeta_script_result_text) {
        memcpy(__cajeta_script_result_text, text, n);
    }
}

// Borrowed, valid until the next store or clear. Null means the cell had no
// trailing expression value at all.
const char* __cajeta_script_result_get(void) {
    if (!__cajeta_script_result_present) return 0;
    return __cajeta_script_result_text ? __cajeta_script_result_text : "";
}

// --- session-scoped fault containment (jupyter-kernel spec 4.4) ----------
//
// `__cajeta_throw` with no exception frame installed calls exit(1) — right
// for a program, fatal for a kernel: an uncaught throw in one cell would take
// the whole session, every binding, and every earlier cell with it. The
// remedy is to give the cell boundary a frame of its own, so a throw that
// escapes the cell unwinds HERE instead of out of the process.
//
// This is the resident-debug-server deferral, paid.

// Run `entry` with a session-level catch installed. Returns NULL when the
// call completed (its value in *out_value), or the thrown Throwable.
// jupyter-kernel U6 (spec 5.1) — interrupting a running cell.
//
// The thrown "value" for an interrupt is a SENTINEL ADDRESS, not a Throwable.
// Nothing dereferences it: the guard below recognizes it by identity before
// it reaches `__cajeta_is_unrecoverable`, and the kernel renders it from the
// pointer alone. Constructing a real exception object would mean calling into
// compiled Cajeta code from a safepoint that may be inside anything.
// External for the same reason the interrupt flag is (see cajeta_rt_core.c):
// the marker accessor and the guard's identity test must agree on ONE
// address, and an internal global can be duplicated across JIT partitions —
// which here would mean an interrupt the guard fails to recognize and then
// dereferences as a Throwable.
char __cajeta_session_interrupt_sentinel = 0;

// The frame an interrupt unwinds TO: the one `__cajeta_session_guard_call`
// pushed for the cell currently running, or NULL when no cell is. Set and
// restored by the guard, so it is always a frame whose stack is still live —
// the property "the outermost link of the chain" does not have.
//
// A plain pointer rather than TLS: a session has exactly one execution
// thread by contract (KernelSession.h), so there is exactly one guarded cell
// at a time, and a `__thread` here would risk the internal-global
// duplication the interrupt flag already had to be made external to avoid.
struct cajeta_exception_frame* __cajeta_session_guard_frame = NULL;

void* __cajeta_session_interrupt_marker(void) {
    return &__cajeta_session_interrupt_sentinel;
}

// Unwind to the OUTERMOST exception frame on this thread's chain — the
// session guard's — not the innermost, which is what `__cajeta_throw` would
// do. Two reasons, and both matter:
//
//   * A cell's own `try { } catch (Exception e)` must not swallow an
//     interrupt. It would bind the sentinel as an object and dereference it,
//     and "Ctrl-C got eaten by a catch-all in cell 3" is not a thing a
//     notebook user can debug.
//   * The guard is where the cell's error is turned into a reply. Landing
//     anywhere else means the interrupt is reported by code that does not
//     know it happened.
//
// Returns normally when there is no frame to land in — a cell running outside
// a guard is not interruptible, which is the honest answer rather than a
// longjmp into nothing.
void __cajeta_session_interrupt_unwind(void) {
    // The guard's OWN frame, recorded by the guard. Emphatically not "walk
    // the chain to its outermost link", which was the first attempt: the
    // outermost link is not necessarily live. A frame left by an earlier,
    // completed call has a `jmp_buf` describing a stack that no longer
    // exists, and longjmping into it faults immediately — which is exactly
    // what happened, and it happened with all chain repair disabled, which is
    // how the longjmp itself was identified as the culprit rather than the
    // bookkeeping around it.
    struct cajeta_exception_frame* outer = __cajeta_session_guard_frame;
    if (!outer) return;
    struct cajeta_exception_frame** excTop = __cajeta_exc_top_ptr();
    if (!*excTop) return;

    // Run the drops the skipped frames own, exactly as __cajeta_throw does on
    // its way to a catch: an interrupt must not leak what the cell allocated.
    struct cajeta_drop_entry** dropTop = __cajeta_drop_top_ptr();
    struct cajeta_drop_entry* watermark = outer->drop_watermark;
    while (*dropTop != watermark) {
        struct cajeta_drop_entry* e = *dropTop;
        if (!e) break;
        if (e->active && e->drop_fn) e->drop_fn(e->obj);
        *dropTop = e->prev;
    }
    // The unwound frames ran no __cajeta_line_leave / __cajeta_dbg_frame_leave,
    // so restore both chains to the guard's watermarks — the same repair
    // __cajeta_throw performs, and for the same reason: a leaked frame makes
    // every later stack trace and step depth wrong.
    __cajeta_shadow_set_top(outer->shadow_watermark);
    {
        struct cajeta_dbg_frame** dbgTop = __cajeta_dbg_top_ptr();
        struct cajeta_dbg_frame* mark = outer->dbg_watermark;
        int g = 0;
        while (*dbgTop && *dbgTop != mark && g++ < 65536) {
            struct cajeta_dbg_frame* f = *dbgTop;
            *dbgTop = f->prev;
            if (f->owner == dbgTop) free(f);
        }
    }
    *excTop = outer;
    // Through the ACCESSOR, never `&sentinel` directly — and the guard reads
    // it the same way. A global's address is only a reliable identity if
    // every party sees the same copy, and this runtime is materialized by the
    // JIT in partitions where an internal global can be duplicated. Going via
    // the one function symbol makes both sides agree by construction. Getting
    // this wrong does not misreport the interrupt, it CRASHES: the guard
    // fails the identity test, decides the sentinel is a Throwable, and
    // dereferences it.
    outer->thrown_value = __cajeta_session_interrupt_marker();
    longjmp(outer->buf, 1);
}

void* __cajeta_session_guard_call(int32_t (*entry)(void), int32_t* out_value) {
    // Anything read after the longjmp has to survive it: a non-volatile local
    // modified between setjmp and longjmp is indeterminate, and a parameter
    // may live in a caller-saved register.
    int32_t (* volatile fn)(void) = entry;
    int32_t* volatile outp = out_value;
    void* volatile scopeMark = __cajeta_scope_save_top();

    struct cajeta_exception_frame frame;
    __cajeta_exc_push(&frame);
    // Publish this frame as the interrupt target while the cell runs, and put
    // back whatever was there on the way out (nesting stays honest even
    // though the kernel never nests). U6.
    struct cajeta_exception_frame* volatile priorGuard =
        __cajeta_session_guard_frame;
    __cajeta_session_guard_frame = &frame;
    if (setjmp(frame.buf) == 0) {
        int32_t v = fn();
        __cajeta_session_guard_frame = priorGuard;
        __cajeta_exc_pop();
        if (outp) *outp = v;
        return NULL;
    }
    __cajeta_session_guard_frame = priorGuard;
    // Landed from __cajeta_throw's longjmp. It has already unwound the drop
    // chain to this frame's watermark and restored the line/debug chains; the
    // value is parked in the frame.
    void* thrown = frame.thrown_value;
    __cajeta_exc_pop();
    // An INTERRUPT is a sentinel address, not a Throwable — recognized here,
    // before anything can dereference it. It still needs the drain below (the
    // cell may have stranded spawned work), so it falls through rather than
    // returning early.
    int interrupted = (thrown == __cajeta_session_interrupt_marker());
    // A PANIC is not a cell error. The guard is a catch-all, so without this
    // an UnrecoverableException — reserved for invariant violations — would
    // be swallowed into a red cell and the session would carry on over a
    // world it has already been told is broken. Same emit-and-abort the
    // no-frame path takes (jupyter-kernel 4.3.1).
    if (!interrupted && __cajeta_is_unrecoverable(thrown)) {
        __cajeta_emit_uncaught(thrown, /*is_unrec=*/1);
        abort();
    }
    // Join and cancel whatever the throw stranded — the work a CATCHING
    // Cajeta function does on its way out via __cajeta_scope_exit_to. Under
    // its own guard, because draining can re-raise a child's trigger, and
    // that must not reach the process-level uncaught path either.
    {
        struct cajeta_exception_frame drain;
        __cajeta_exc_push(&drain);
        if (setjmp(drain.buf) == 0) {
            __cajeta_scope_exit_to((void*) scopeMark);
        }
        __cajeta_exc_pop();
    }
    return thrown;
}

// The thrown value's canonical class name, via
// obj -> vtable -> classObject -> rtti. "" when the value is not a real
// object (a legacy int throw arrives here as a small integer cast to a
// pointer, and must not be dereferenced).
const char* __cajeta_throwable_type(void* v) {
    if (!v || (uintptr_t) v < 4096) return "";
    void* vtable = *(void**) v;
    if (!vtable || (uintptr_t) vtable < 4096) return "";
    void* classObject =
        *(void**) ((char*) vtable + CAJETA_VTABLE_CLASSOBJECT_OFFSET);
    if (!classObject || (uintptr_t) classObject < 4096) return "";
    void* rtti = *(void**) ((char*) classObject + 8);
    return __cajeta_rtti_type_name(rtti);
}

// Throwable.message into a caller buffer, NUL-terminated. Returns the number
// of bytes written (0 when there is no message). Same layout walk as the
// uncaught-throw emitter — Throwable{vtable@0, String message@8} — and the
// same guards, so a non-Throwable value yields 0 rather than a fault.
int32_t __cajeta_throwable_message_into(void* v, char* out, int32_t cap) {
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    if (!v || (uintptr_t) v < 4096) return 0;
    void* strObj = ((void**) v)[1];
    if (!strObj || (uintptr_t) strObj < 4096) return 0;
    int32_t lt = *(int32_t*) ((char*) strObj + 8);
    int32_t blen = lt & 0x1FFFFFFF;
    if (blen <= 0) return 0;
    const char* bytes;
    if (blen <= 12) {
        bytes = (const char*) strObj + 12;
    } else {
        int32_t soff = *(int32_t*) ((char*) strObj + 12);
        char* sbase = *(char**) ((char*) strObj + 16);
        if (!sbase || (uintptr_t) sbase < 4096) return 0;
        bytes = sbase + 8 + soff;
    }
    if (blen > cap - 1) blen = cap - 1;
    memcpy(out, bytes, (size_t) blen);
    out[blen] = 0;
    return blen;
}

// How many SEMANTIC frames the throw captured (innermost first). Zero when
// line-info capture was off — the throwable still carries its type and
// message, so an error payload is never empty for want of a trace.
int32_t __cajeta_throwable_frame_count(void* v) {
    pthread_mutex_lock(&__cajeta_trace_mutex);
    struct cajeta_trace_entry* e = __cajeta_trace_table;
    while (e && e->throwable != v) e = e->next;
    int32_t n = (e && e->shadow) ? (int32_t) e->shadow_count : 0;
    pthread_mutex_unlock(&__cajeta_trace_mutex);
    return n;
}

// One frame, as borrowed pointers into the module-lived frame descriptors —
// valid for the process's life, so the caller may read them after unlocking.
// Returns 1 on success, 0 when `idx` is out of range.
int32_t __cajeta_throwable_frame(void* v, int32_t idx, const char** type,
                                 const char** method, const char** file,
                                 int32_t* line) {
    pthread_mutex_lock(&__cajeta_trace_mutex);
    struct cajeta_trace_entry* e = __cajeta_trace_table;
    while (e && e->throwable != v) e = e->next;
    if (!e || !e->shadow || idx < 0 || idx >= e->shadow_count) {
        pthread_mutex_unlock(&__cajeta_trace_mutex);
        return 0;
    }
    const CajetaFrameDesc* d = e->shadow[idx].desc;
    if (type)   *type   = (d && d->typeName)   ? d->typeName   : "";
    if (method) *method = (d && d->methodName) ? d->methodName : "";
    if (file)   *file   = (d && d->fileName)   ? d->fileName   : "";
    if (line)   *line   = e->shadow[idx].line;
    pthread_mutex_unlock(&__cajeta_trace_mutex);
    return 1;
}
