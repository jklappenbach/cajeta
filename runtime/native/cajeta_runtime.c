// Cajeta language runtime — compiled to LLVM bitcode at compiler build time,
// embedded into the compiler binary, and linker-merged into every user module.
//
// Keep these helpers small and pointer-only at their ABI boundary; the optimizer
// inlines and specializes them across user code.

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef void (*cajeta_ctor_fn)(void* self);

// Allocate and zero-fill a buffer holding total_count elements of elem_size bytes.
// Used for primitive-element arrays.
void* __cajeta_new_array(uint64_t elem_size, uint64_t total_count) {
    if (total_count == 0) {
        return NULL;
    }
    void* buf = calloc((size_t) total_count, (size_t) elem_size);
    if (buf == NULL) {
        fprintf(stderr, "cajeta: __cajeta_new_array failed (count=%llu, size=%llu)\n",
                (unsigned long long) total_count, (unsigned long long) elem_size);
        abort();
    }
    return buf;
}

// Same as above, then run `ctor` on each element. Used for class-element arrays.
void* __cajeta_new_class_array(uint64_t elem_size, uint64_t total_count, cajeta_ctor_fn ctor) {
    void* buf = __cajeta_new_array(elem_size, total_count);
    if (buf == NULL || ctor == NULL) {
        return buf;
    }
    char* p = (char*) buf;
    for (uint64_t i = 0; i < total_count; i++) {
        ctor(p);
        p += elem_size;
    }
    return buf;
}

// Allocate a Java-style array header — { i64 size, [count x elem] data } — laid
// out as one contiguous heap block. Stores `count` into the size field at offset 0
// and zero-fills the data region. The compiler emits one call per array level for
// multi-dim shapes (outer first, then per-element inner allocations).
//
// header_size is the offset of the data region (typically 8 for an i64 size field
// with no padding, but the compiler queries DataLayout::getTypeAllocSize on the
// header struct to be safe under alignment).
void* __cajeta_new_array_header(uint64_t header_size, uint64_t elem_size, uint64_t count) {
    uint64_t total = header_size + count * elem_size;
    if (total == 0) {
        return NULL;
    }
    void* hdr = calloc(1, (size_t) total);
    if (hdr == NULL) {
        fprintf(stderr, "cajeta: __cajeta_new_array_header failed (header=%llu elem=%llu count=%llu)\n",
                (unsigned long long) header_size,
                (unsigned long long) elem_size,
                (unsigned long long) count);
        abort();
    }
    // Store count at the size field (first 8 bytes of the header).
    *((int64_t*) hdr) = (int64_t) count;
    return hdr;
}

void __cajeta_free_array(void* ptr) {
    free(ptr);
}

// Generic zero-fill allocation for compiler-emitted heap blocks that don't
// match an array shape — used for closure records and captures structs in
// L3-3. Mirrors __cajeta_new_array's failure mode so the compiler doesn't
// have to handle null returns.
void* __cajeta_alloc(uint64_t size) {
    if (size == 0) return NULL;
    void* p = calloc(1, (size_t) size);
    if (p == NULL) {
        fprintf(stderr, "cajeta: __cajeta_alloc failed (size=%llu)\n",
                (unsigned long long) size);
        abort();
    }
    return p;
}

// Mirror of __cajeta_free_array for non-array heap blocks. Kept as a
// separate symbol so the drop-fn function-pointer types match what the
// emitted IR uses for arrays (both are `void(*)(void*)`).
void __cajeta_free(void* ptr) {
    free(ptr);
}

// Drop dispatcher for function-typed locals. The drop chain registers
// this generic helper for every function-typed local; at scope exit it
// reads the closure record's drop_fn slot and invokes it if non-null.
// Non-capturing closures (whose record is a global constant with
// drop_fn=null) and null pointers are no-ops, so the same drop-entry
// shape is safe for every assignment.
//
// Closure record layout (L3-3): { void* fn, void* captures, void(*drop_fn)(void*) }
struct cajeta_closure_record {
    void* fn;
    void* captures;
    void (*drop_fn)(void*);
};

void __cajeta_closure_drop(void* p) {
    if (!p) return;
    struct cajeta_closure_record* c = (struct cajeta_closure_record*) p;
    if (c->drop_fn) c->drop_fn(p);
}

// --- Threading sync primitives: Lock --------------------------------------
//
// Cajeta's `Lock` is the no-data RAII gate from ThreadModel.md. The OS-level
// implementation is just a pthread_mutex_t — the language-side guard
// semantics (drop-on-scope-exit) layer on top. Async-aware suspension also
// layers on top, once the executor exists; for v0 these are blocking calls
// against the OS mutex, which is enough for single-thread tests and for
// the future user-facing Lock class to wrap.
//
// The intrinsic-level API is a deliberate stepping stone: Cajeta source
// invokes `Cajeta.lockNew / lockAcquire / lockRelease / lockTryAcquire /
// lockDestroy` directly via the namespace-dispatch path in
// MethodCallExpression. Once user-defined class drop lands, a `Lock` class
// will wrap these calls with an `acquire()` that returns a `LockGuard`
// whose drop calls release.

void* __cajeta_lock_new(void) {
    pthread_mutex_t* m = (pthread_mutex_t*) malloc(sizeof(pthread_mutex_t));
    if (!m) {
        fprintf(stderr, "cajeta: __cajeta_lock_new failed\n");
        abort();
    }
    if (pthread_mutex_init(m, NULL) != 0) {
        fprintf(stderr, "cajeta: pthread_mutex_init failed\n");
        free(m);
        abort();
    }
    return m;
}

void __cajeta_lock_acquire(void* p) {
    if (!p) return;
    pthread_mutex_lock((pthread_mutex_t*) p);
}

void __cajeta_lock_release(void* p) {
    if (!p) return;
    pthread_mutex_unlock((pthread_mutex_t*) p);
}

// Returns 1 if the lock was acquired, 0 if it was already held. Mirrors
// the boolean return shape that the eventual `tryAcquire(): bool` method
// in the user-facing Lock class will surface.
int32_t __cajeta_lock_try_acquire(void* p) {
    if (!p) return 0;
    return pthread_mutex_trylock((pthread_mutex_t*) p) == 0 ? 1 : 0;
}

void __cajeta_lock_destroy(void* p) {
    if (!p) return;
    pthread_mutex_destroy((pthread_mutex_t*) p);
    free(p);
}

// --- Threading: async task queue + worker (R2) ----------------------------
//
// ThreadModel.md sync primitives have an OS implementation here. The async-
// runtime side: spawn enqueues a (trampoline, task) pair; a worker thread
// pops + invokes the trampoline. The trampoline is codegen-emitted per
// spawn site: it calls the user's async fn, writes the result into the
// task's value slot, then calls __cajeta_task_complete with the address of
// the task's `done` field to flip it under the mutex + broadcast.
//
// R2 is one worker thread, one global queue, one global mutex/condvar pair
// per state (queue-non-empty, any-task-done). A real work-stealing pool +
// per-task wait queues lands later. Today's design is enough for correct
// (if not maximally efficient) concurrency: the main thread's await blocks
// on the global done-condvar; the worker writes done + broadcasts.

typedef void (*cajeta_task_trampoline_fn)(void* task);

struct cajeta_task_work {
    cajeta_task_trampoline_fn trampoline;
    void* task;
    struct cajeta_task_work* next;
};

static pthread_mutex_t __cajeta_task_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  __cajeta_task_queue_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  __cajeta_task_done_cond  = PTHREAD_COND_INITIALIZER;
static struct cajeta_task_work* __cajeta_task_queue_head = NULL;
static struct cajeta_task_work* __cajeta_task_queue_tail = NULL;
static int __cajeta_task_workers_started = 0;
static pthread_t __cajeta_task_worker;

static void* __cajeta_task_worker_loop(void* arg) {
    (void) arg;
    for (;;) {
        pthread_mutex_lock(&__cajeta_task_mutex);
        while (!__cajeta_task_queue_head) {
            pthread_cond_wait(&__cajeta_task_queue_cond, &__cajeta_task_mutex);
        }
        struct cajeta_task_work* w = __cajeta_task_queue_head;
        __cajeta_task_queue_head = w->next;
        if (!__cajeta_task_queue_head) __cajeta_task_queue_tail = NULL;
        pthread_mutex_unlock(&__cajeta_task_mutex);
        w->trampoline(w->task);
        free(w);
    }
    return NULL;
}

// Enqueue a trampoline/task pair. Starts the worker thread on first call —
// lazy init avoids paying a thread-create cost for programs that never
// spawn. The trampoline is codegen-emitted; the runtime is type-agnostic
// (just a function pointer).
void __cajeta_task_run(void* task, cajeta_task_trampoline_fn trampoline) {
    pthread_mutex_lock(&__cajeta_task_mutex);
    if (!__cajeta_task_workers_started) {
        __cajeta_task_workers_started = 1;
        pthread_create(&__cajeta_task_worker, NULL,
                       __cajeta_task_worker_loop, NULL);
    }
    struct cajeta_task_work* w = malloc(sizeof(*w));
    if (!w) {
        fprintf(stderr, "cajeta: __cajeta_task_run malloc failed\n");
        pthread_mutex_unlock(&__cajeta_task_mutex);
        abort();
    }
    w->trampoline = trampoline;
    w->task = task;
    w->next = NULL;
    if (__cajeta_task_queue_tail) {
        __cajeta_task_queue_tail->next = w;
        __cajeta_task_queue_tail = w;
    } else {
        __cajeta_task_queue_head = w;
        __cajeta_task_queue_tail = w;
    }
    pthread_cond_signal(&__cajeta_task_queue_cond);
    pthread_mutex_unlock(&__cajeta_task_mutex);
}

// Block the calling thread until the task at `done_addr` flips to nonzero.
// Codegen passes `&task->done` directly so the runtime doesn't need the
// CajetaTask layout's field offset.
void __cajeta_task_wait(int32_t* done_addr) {
    if (!done_addr) return;
    pthread_mutex_lock(&__cajeta_task_mutex);
    while (!*done_addr) {
        pthread_cond_wait(&__cajeta_task_done_cond, &__cajeta_task_mutex);
    }
    pthread_mutex_unlock(&__cajeta_task_mutex);
}

// Called by the codegen-emitted trampoline once the inner fn has run and
// its result has been written into the task's value slot. Sets done = 1
// under the mutex (so it pairs with __cajeta_task_wait's predicate check)
// and broadcasts to wake every awaiter.
void __cajeta_task_complete(int32_t* done_addr) {
    if (!done_addr) return;
    pthread_mutex_lock(&__cajeta_task_mutex);
    *done_addr = 1;
    pthread_cond_broadcast(&__cajeta_task_done_cond);
    pthread_mutex_unlock(&__cajeta_task_mutex);
}

// Abort with a diagnostic when an array index is out of bounds. Compiler emits a
// conditional branch to this from ArrayIndexExpression when bounds checking is on.
void __cajeta_array_bounds_fail(int64_t index, int64_t dim) {
    fprintf(stderr, "cajeta: array index %lld out of bounds for dimension size %lld\n",
            (long long) index, (long long) dim);
    abort();
}

// --- exception handling (setjmp/longjmp-based) -------------------------------
//
// Each try-block allocates a `cajeta_exception_frame` on the stack and registers
// it with __cajeta_exc_push. `throw` writes the value into the topmost frame and
// longjmps back to its setjmp point. The compiler emits the setjmp call inline
// (it must run in the caller's frame), so the runtime never sees it directly.
//
// Single-threaded for now — the frame stack is a plain global. Promoting to
// __thread / _Thread_local is a small change but unnecessary until JIT'd code
// runs on multiple threads.

// --- drop chain (Session 3 of the memory-model rollout) ---------------------
//
// Owners declared in a function push a `cajeta_drop_entry` onto a per-thread
// linked list at declaration time, and pop+drop at scope exit. The exception
// throw path walks this chain down to the catching try-frame's watermark so
// stack unwinding fires drops along the way. See MemoryModel.md § Runtime:
// drop chain with watermark.

struct cajeta_drop_entry {
    void* obj;
    void (*drop_fn)(void*);
    struct cajeta_drop_entry* prev;
    int8_t active;  // i8 instead of bool — fixed ABI for the IR side
};

size_t __cajeta_drop_entry_size(void) {
    return sizeof(struct cajeta_drop_entry);
}

static struct cajeta_drop_entry* __cajeta_drop_top = NULL;

// Observability for tests: bumped every time a drop function actually fires
// (i.e. an active entry pops or unwinds). Tests can read it to assert that
// owned resources are freed at expected program points.
static int64_t __cajeta_drop_count = 0;

int64_t __cajeta_drop_count_get(void) { return __cajeta_drop_count; }
void __cajeta_drop_count_reset(void) { __cajeta_drop_count = 0; }

// Push an owner onto the drop chain. The entry storage is stack-allocated in
// the caller's frame; we never own the memory, only chain pointers through it.
void __cajeta_drop_push(struct cajeta_drop_entry* e, void* obj, void (*drop_fn)(void*)) {
    e->obj = obj;
    e->drop_fn = drop_fn;
    e->prev = __cajeta_drop_top;
    e->active = 1;
    __cajeta_drop_top = e;
}

// Pop the topmost entry and run its drop function if still active. Caller
// passes the entry pointer so popping can verify shape (in debug builds; v1
// trusts the caller).
void __cajeta_drop_pop_run(struct cajeta_drop_entry* e) {
    if (e->active && e->drop_fn) {
        __cajeta_drop_count++;
        e->drop_fn(e->obj);
    }
    __cajeta_drop_top = e->prev;
}

// Mark an entry inactive (the owner has been moved out via `#`). The entry
// remains on the chain so scope-exit pop logic still finds it, but the drop
// function won't run.
void __cajeta_drop_mark_inactive(struct cajeta_drop_entry* e) {
    e->active = 0;
}

// --- VTable: hash-based dispatch ---------------------------------------------
//
// Each class's vtable is a sorted array of (signature-hash, function-pointer)
// entries. Dispatch hashes the call-site's method canonical signature, binary-
// searches the receiver's vtable, and indirect-calls the matching function.
// This sidesteps the slot-index collision problem that single-vtable layouts
// run into for multiple inheritance — methods are addressed by stable hash,
// not by position.
//
// Layout (LLVM struct):
//   { i16 version, i16 count, [count x { i64 hash, ptr fn }] entries }
//
// The header is 4 bytes (`version` + `count`), but the entries array is
// 8-byte aligned per LLVM's default rules — so there are 4 bytes of padding
// before `entries`, and the entries themselves start at byte offset 8.

struct cajeta_vtable_entry {
    int64_t hash;
    void* fn;
};

// FNV-1a 64-bit hash. Stable across runs and platforms — both the compiler
// (at vtable build time) and the runtime (at dispatch time) compute the
// same hash for the same canonical signature.
int64_t __cajeta_signature_hash(const char* s) {
    if (!s) return 0;
    uint64_t h = 0xcbf29ce484222325ULL;     // FNV offset basis
    while (*s) {
        h ^= (uint8_t) *s++;
        h *= 0x100000001b3ULL;              // FNV prime
    }
    return (int64_t) h;
}

// Binary-search the vtable for `hash`; return the matching function pointer
// or NULL if not found. The "not found" case shouldn't happen for well-typed
// dispatch — the static type guarantees the method exists on the receiver —
// but is treated as a soft miss so a misuse aborts at the call site (NULL
// fn-pointer call) rather than corrupting memory.
void* __cajeta_vtable_lookup(void* vptr, int64_t hash) {
    if (!vptr) return NULL;
    const int16_t* hdr = (const int16_t*) vptr;
    int32_t count = hdr[1];                 // slot 1 = count
    if (count <= 0) return NULL;
    const struct cajeta_vtable_entry* entries =
        (const struct cajeta_vtable_entry*) ((const char*) vptr + 8);
    int lo = 0, hi = count;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        int64_t h = entries[mid].hash;
        if (h < hash) lo = mid + 1;
        else if (h > hash) hi = mid;
        else return entries[mid].fn;
    }
    return NULL;
}

struct cajeta_exception_frame {
    jmp_buf buf;
    struct cajeta_exception_frame* prev;
    int64_t thrown_value;
    // Drop-chain watermark snapshotted at try-entry. On throw, the runtime
    // unwinds drops between the current top and this watermark before longjmp.
    struct cajeta_drop_entry* drop_watermark;
};

// Exposed as a compile-time-known size for the IR side; the compiler allocates a
// blob of this size for each try-frame. Using a fixed 512-byte buffer in IR is
// portable enough for x86-64 and aarch64 glibc/musl, but we expose the actual
// size here so the JIT helper can sanity-check.
size_t __cajeta_exc_frame_size(void) {
    return sizeof(struct cajeta_exception_frame);
}

static struct cajeta_exception_frame* __cajeta_exc_top = NULL;

void __cajeta_exc_push(struct cajeta_exception_frame* f) {
    f->prev = __cajeta_exc_top;
    f->thrown_value = 0;
    // Snapshot the current drop-chain top so a throw can unwind back to here.
    f->drop_watermark = __cajeta_drop_top;
    __cajeta_exc_top = f;
}

void __cajeta_exc_pop(void) {
    if (__cajeta_exc_top) {
        __cajeta_exc_top = __cajeta_exc_top->prev;
    }
}

__attribute__((noreturn))
void __cajeta_throw(int64_t value) {
    if (!__cajeta_exc_top) {
        fprintf(stderr, "cajeta: uncaught exception (value=%lld)\n",
                (long long) value);
        abort();
    }
    // Unwind drops between the current top and the catching frame's watermark.
    // Each active entry runs its drop function once; the entry itself is
    // stack-allocated in the originating frame, so we only manipulate the
    // chain pointer here.
    struct cajeta_drop_entry* watermark = __cajeta_exc_top->drop_watermark;
    while (__cajeta_drop_top != watermark) {
        struct cajeta_drop_entry* e = __cajeta_drop_top;
        if (!e) break;  // shouldn't happen, but bail rather than loop
        if (e->active && e->drop_fn) {
            __cajeta_drop_count++;
            e->drop_fn(e->obj);
        }
        __cajeta_drop_top = e->prev;
    }
    __cajeta_exc_top->thrown_value = value;
    longjmp(__cajeta_exc_top->buf, 1);
}

int64_t __cajeta_get_thrown(void) {
    return __cajeta_exc_top ? __cajeta_exc_top->thrown_value : 0;
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

#include <time.h>

// System.exit(code). Terminates the process — caller must not rely on return.
__attribute__((noreturn))
void __cajeta_exit(int32_t code) {
    // Use _Exit so atexit handlers (incl. stdio buffer flush) don't run; matches
    // Java's exit semantics where the JVM is torn down without C-style cleanup.
    _Exit(code);
}

// System.currentTimeMillis(). Wall-clock ms since the Unix epoch.
int64_t __cajeta_currentTimeMillis(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (int64_t) ts.tv_sec * 1000 + (int64_t) (ts.tv_nsec / 1000000);
}

// Math.random() — pseudo-random double in [0.0, 1.0).
// Seeded lazily from the wall clock on first call so independent runs differ;
// concurrent callers race on the seed but the resulting numbers are still
// uniformly distributed in expectation, which is good enough for typical use.
double __cajeta_random(void) {
    static int seeded = 0;
    if (!seeded) {
        seeded = 1;
        srand((unsigned) __cajeta_currentTimeMillis());
    }
    return (double) rand() / ((double) RAND_MAX + 1.0);
}

// substring(begin, end) — like Java's String.substring, half-open. Out-of-range
// indices clamp to the valid window; the result is always a freshly malloc'd
// null-terminated copy (so callers can pass it back to concat etc.).
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
