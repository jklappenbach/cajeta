// Session-binding registry: a script unit's top-level bindings are owned by the
// SESSION rather than the synthesized entry's drop frame, so they survive its
// return. The host ends the session. Single-threaded by contract; no locking.

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

// The slot bound to `name`, or null.
static cajeta_session_slot* cajeta_session_find(const char* name) {
    for (int64_t i = 0; i < __cajeta_session_len; ++i) {
        if (strcmp(__cajeta_session_slots[i].name, name) == 0) {
            return &__cajeta_session_slots[i];
        }
    }
    return 0;
}

// Bind `obj` under `name`, taking ownership. A rebind drops the old occupant NOW
// and keeps the name's ORIGINAL position, so drop_all stays reverse first-bind.
void __cajeta_session_bind(const char* name, void* obj,
                           void (*drop_fn)(void*)) {
    cajeta_session_slot* slot = cajeta_session_find(name);
    if (slot) {
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

// Drop for a boxed primitive: the box is a plain malloc'd buffer.
static void cajeta_session_free_box(void* p) {
    free(p);
}

// Bind a PRIMITIVE by value: its frame slot dies with the entry, so `size` bytes
// are copied into a session-owned box, which `__cajeta_session_get` returns.
void __cajeta_session_bind_value(const char* name, const void* src,
                                 int64_t size) {
    if (!src || size <= 0) return;
    void* box = malloc((size_t) size);
    if (!box) return;  // OOM: leave the previous binding intact
    memcpy(box, src, (size_t) size);
    __cajeta_session_bind(name, box, cajeta_session_free_box);
}

// Ownership left the session (a `#` transfer): quiet the slot WITHOUT dropping,
// since the new owner drops. A later rebind reoccupies the same slot.
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

// Drop every live binding in reverse first-binding order and empty the registry.
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

// --- the unit RESULT: Out[N] ---------------------------------------------
// A cell's trailing expression is rendered to text by CODEGEN and parked here.
// "No result" and "an empty rendering" differ, so presence is tracked apart.
static char* __cajeta_script_result_text = 0;
static int __cajeta_script_result_present = 0;

void __cajeta_script_result_clear(void) {
    free(__cajeta_script_result_text);
    __cajeta_script_result_text = 0;
    __cajeta_script_result_present = 0;
}

// Park a COPY of `text`, which the caller keeps. NULL is "nothing rendered" and
// leaves what is parked; an empty non-null string is a real, replacing result.
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

// Borrowed until the next store or clear; null when the cell had no result.
const char* __cajeta_script_result_get(void) {
    if (!__cajeta_script_result_present) return 0;
    return __cajeta_script_result_text ? __cajeta_script_result_text : "";
}

// --- session-scoped fault containment -------------------------------------

// An interrupt's thrown "value" is a SENTINEL ADDRESS, never dereferenced.
// EXTERNAL: the JIT duplicates internal globals, and identity needs one address.
char __cajeta_session_interrupt_sentinel = 0;

// The frame an interrupt unwinds TO: the one the guard pushed for the running
// cell, or NULL. Always live, which the chain's outermost link is not.
struct cajeta_exception_frame* __cajeta_session_guard_frame = NULL;

void* __cajeta_session_interrupt_marker(void) {
    return &__cajeta_session_interrupt_sentinel;
}

// The other stop, and the other sentinel: a would-be-UB trap (divide by zero,
// shift past the width, signed overflow) that must not `llvm.trap` the session.
char __cajeta_session_trap_sentinel = 0;
static const char* g_trap_what = "arithmetic fault";

void* __cajeta_session_trap_marker(void) {
    return &__cajeta_session_trap_sentinel;
}

// What the trap was, as a string LITERAL owned by the emitting module.
const char* __cajeta_session_trap_description(void) {
    return g_trap_what;
}

// Park `marker`, the sentinel the guard compares by identity, and unwind to the
// SESSION GUARD's frame, not the innermost one `__cajeta_throw` would take: a
// cell's catch-all must not swallow a stop. Returns when no cell is guarded.
static void session_unwind_to_guard(void* marker) {
    struct cajeta_exception_frame* outer = __cajeta_session_guard_frame;
    if (!outer) return;
    struct cajeta_exception_frame** excTop = __cajeta_exc_top_ptr();
    if (!*excTop) return;

    // Run the drops the skipped frames own, as __cajeta_throw does on its way.
    struct cajeta_drop_entry** dropTop = __cajeta_drop_top_ptr();
    struct cajeta_drop_entry* watermark = outer->drop_watermark;
    while (*dropTop != watermark) {
        struct cajeta_drop_entry* e = *dropTop;
        if (!e) break;
        if (e->active && e->drop_fn) e->drop_fn(e->obj);
        *dropTop = e->prev;
    }
    // The unwound frames ran no line/debug leave; a leaked one skews every trace.
    __cajeta_shadow_set_top(outer->shadow_watermark);
    __cajeta_prof_instr_set_depth(outer->instr_watermark);
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
    // `marker` came through the ACCESSOR: a failed identity test dereferences it.
    outer->thrown_value = marker;
    longjmp(outer->buf, 1);
}

// Stop the running cell with the interrupt sentinel.
void __cajeta_session_interrupt_unwind(void) {
    session_unwind_to_guard(__cajeta_session_interrupt_marker());
}

// Called from a would-be-UB trap site instead of `llvm.trap` under a session;
// `what` is what the host renders. RETURNS NORMALLY when no cell is guarded.
void __cajeta_session_trap_unwind(const char* what) {
    if (what) g_trap_what = what;
    session_unwind_to_guard(__cajeta_session_trap_marker());
}

// The guard's captures must be NON-UNWINDING on x86-64 Windows: MSVCRT's longjmp
// SEH-unwinds to the capture, and the COFF JIT drops the cell frames' unwind
// tables, so it walks unregistered frames. `_setjmp(buf, NULL)` opts out.
#if defined(_WIN32) && defined(__x86_64__)
#define CAJETA_EXC_SETJMP(buf) _setjmp((buf), (void*) 0)
#else
#define CAJETA_EXC_SETJMP(buf) setjmp(buf)
#endif

// Run `entry` under a session-level catch, so a throw escaping the cell lands
// here instead of exiting the process. Returns NULL when the call completed (its
// value in *out_value), else the thrown value — a Throwable or a stop sentinel.
void* __cajeta_session_guard_call(int32_t (*entry)(void), int32_t* out_value) {
    // Read after the longjmp: a non-volatile local written before it is indeterminate.
    int32_t (* volatile fn)(void) = entry;
    int32_t* volatile outp = out_value;
    void* volatile scopeMark = __cajeta_scope_save_top();

    struct cajeta_exception_frame frame;
    __cajeta_exc_push(&frame);
    struct cajeta_exception_frame* volatile priorGuard =
        __cajeta_session_guard_frame;
    __cajeta_session_guard_frame = &frame;
    if (CAJETA_EXC_SETJMP(frame.buf) == 0) {
        int32_t v = fn();
        __cajeta_session_guard_frame = priorGuard;
        __cajeta_exc_pop();
        if (outp) *outp = v;
        return NULL;
    }
    __cajeta_session_guard_frame = priorGuard;
    void* thrown = frame.thrown_value;
    __cajeta_exc_pop();
    // Recognized before anything can dereference it, but still drained below.
    int interrupted = (thrown == __cajeta_session_interrupt_marker());
    // A PANIC is not a cell error: this catch-all must not swallow one.
    if (!interrupted && __cajeta_is_unrecoverable(thrown)) {
        __cajeta_emit_uncaught(thrown, /*is_unrec=*/1);
        abort();
    }
    // Join what the throw stranded, guarded: a drain can re-raise a child.
    {
        struct cajeta_exception_frame drain;
        __cajeta_exc_push(&drain);
        if (CAJETA_EXC_SETJMP(drain.buf) == 0) {
            __cajeta_scope_exit_to((void*) scopeMark);
        }
        __cajeta_exc_pop();
    }
    return thrown;
}

// The thrown value's canonical class name, via obj -> vtable -> classObject ->
// rtti; "" when the value is not a real object and must not be dereferenced.
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

// Throwable.message into `out`, NUL-terminated; returns bytes written, 0 for no
// message. Walks Throwable{vtable@0, String message@8} with the emitter's guards.
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

// How many SEMANTIC frames the throw captured (innermost first); zero when
// line-info capture was off, which is not an error.
int32_t __cajeta_throwable_frame_count(void* v) {
    pthread_mutex_lock(&__cajeta_trace_mutex);
    struct cajeta_trace_entry* e = __cajeta_trace_table;
    while (e && e->throwable != v) e = e->next;
    int32_t n = (e && e->shadow) ? (int32_t) e->shadow_count : 0;
    pthread_mutex_unlock(&__cajeta_trace_mutex);
    return n;
}

// Frame `idx` as borrowed pointers into module-lived descriptors, valid for the
// process's life. Returns 1, or 0 when `idx` is out of range.
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

// ---------------------------------------------------------------------
// The Packages install bridge: JIT'd `cajeta.session.Packages` calls in here and
// the host installs the hook; a null hook reports "no live session", not a no-op.

typedef int32_t (*cajeta_install_hook_fn)(const char* name, int32_t nameLen,
                                          const char* constraint,
                                          int32_t constraintLen,
                                          int32_t save,
                                          char* out, int32_t outCap,
                                          void* ctx);

// DEFINED IN THE HOST: this file is compiled TWICE (the compiler binary, and
// every JIT session's bitcode), so a static would give cell code a second copy
// the host's registration never reaches.
extern cajeta_install_hook_fn __cajeta_install_hook;
extern void* __cajeta_install_ctx;
// The resolved version on success, the failure message on failure.
extern char __cajeta_install_out[2048];

extern const char* __cajeta_string_bytes(void* s_v);
extern int32_t __cajeta_string_byte_len(void* s_v);
extern void* __cajeta_string_from_buf(const char* data, int64_t len,
                                      void* vtable);

void __cajeta_session_set_install_hook(cajeta_install_hook_fn fn, void* ctx) {
    __cajeta_install_hook = fn;
    __cajeta_install_ctx = ctx;
}

// Install a package into the live session: 0 = installed (the buffer holds the
// resolved version), non-zero = rejected (the buffer holds the message).
int32_t __cajeta_session_install(void* nameStr, void* conStr, int32_t save) {
    __cajeta_install_out[0] = '\0';
    if (!__cajeta_install_hook) {
        const char* msg = "Packages.install: no live session — installing "
                          "into a running session requires a session host "
                          "(the Jupyter kernel); declare the dependency in "
                          "cajeta.json instead";
        strncpy(__cajeta_install_out, msg, sizeof(__cajeta_install_out) - 1);
        __cajeta_install_out[sizeof(__cajeta_install_out) - 1] = '\0';
        return 1;
    }
    return __cajeta_install_hook(__cajeta_string_bytes(nameStr),
                                 __cajeta_string_byte_len(nameStr),
                                 __cajeta_string_bytes(conStr),
                                 __cajeta_string_byte_len(conStr),
                                 save,
                                 __cajeta_install_out,
                                 (int32_t) sizeof(__cajeta_install_out),
                                 __cajeta_install_ctx);
}

// Wrap the stored install message as a fresh String; `donor` supplies the String
// vtable, which a static native has no receiver to take.
void* __cajeta_session_install_message(void* donor) {
    if (!donor) return 0;
    void* vtable = *(void**) donor;      // vtable is the layout's first field
    return __cajeta_string_from_buf(__cajeta_install_out,
                                    (int64_t) strlen(__cajeta_install_out),
                                    vtable);
}
