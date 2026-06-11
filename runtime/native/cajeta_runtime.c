// Cajeta language runtime — compiled to LLVM bitcode at compiler build time,
// embedded into the compiler binary, and linker-merged into every user module.
//
// Keep these helpers small and pointer-only at their ABI boundary; the optimizer
// inlines and specializes them across user code.

// macOS feature-test macros. Set BEFORE any system header is included.
//
//   - _XOPEN_SOURCE = 600 re-exposes the ucontext.h routines (swapcontext
//     / getcontext / makecontext) that Apple deprecated in 10.6. They
//     still work, just emit -Wdeprecated-declarations warnings. The
//     fiber implementation depends on them; no stackful alternative on
//     macOS until the planned stackless rewrite lands.
//
//   - _DARWIN_C_SOURCE re-exposes BSD extensions that _XOPEN_SOURCE
//     would otherwise hide: flock(2) + LOCK_EX / LOCK_NB / LOCK_UN
//     from <sys/file.h>, O_CLOEXEC from <fcntl.h>, etc. Without it,
//     _XOPEN_SOURCE puts the headers into strict-POSIX mode and the
//     BSD-only entries disappear. Defining both gives us both surfaces.
#if defined(__APPLE__)
#  ifndef _XOPEN_SOURCE
#    define _XOPEN_SOURCE 600
#  endif
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE 1
#  endif
#endif

#include <math.h>      // floorf — CPU texture-sample bilinear/nearest filtering
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>   // write(2) for abort-survivable diagnostics (see below)

// execinfo.h (backtrace + backtrace_symbols) — glibc + macOS only.
// MinGW-w64 doesn't ship it; stub on Windows so the runtime compiles
// there. Stack-trace capture on Windows uses DbgHelp's
// CaptureStackBackTrace + SymFromAddr; that path lands in a later
// release. For v0.1.x, Windows binaries report empty stack traces
// (callers null-check / handle that already).
#if defined(_WIN32)
static int backtrace(void** buf, int max) { (void) buf; (void) max; return 0; }
static char** backtrace_symbols(void* const* buf, int n) { (void) buf; (void) n; return NULL; }

// MinGW provides _commit(fd) in <io.h> as the equivalent of POSIX
// fsync(fd) — sync a file descriptor's data to disk. Macro-substitute
// so the runtime code reads the same on every platform.
#include <io.h>
#include <sys/stat.h>
#define fsync(fd) _commit(fd)

// MinGW's mkdir takes a single path argument (no mode). POSIX mkdir
// takes (path, mode). Provide a uniform call site via a helper macro
// that drops the mode on Windows.
#define cajeta_mkdir(path, mode) mkdir(path)

// lstat / S_ISLNK don't exist on MinGW. Windows handles symlinks via
// reparse points (FILE_ATTRIBUTE_REPARSE_POINT); a faithful port would
// use GetFileAttributesEx and translate. For v0.1.0 we degrade: lstat
// just calls stat (which dereferences symlinks instead of inspecting
// them) and S_ISLNK returns 0 (every path is "not a symlink"). Means
// symlink-aware code paths treat symlinks as their target on Windows.
// Tracked for v0.1.x.
#define lstat(path, statbuf) stat(path, statbuf)
#define S_ISLNK(mode) 0

// realpath -> MinGW provides _fullpath in <stdlib.h>. Passing NULL as
// the buffer allocates one; the caller frees it (same lifetime
// contract as POSIX realpath).
#include <stdlib.h>
#define realpath(in, _ignored) _fullpath(NULL, (in), 0)
#else
#include <execinfo.h>
#define cajeta_mkdir(path, mode) mkdir(path, mode)
#endif

// `malloc_usable_size` ships in different headers per platform, and Apple's
// equivalent is renamed to `malloc_size` (in <malloc/malloc.h>). Conditionalize
// the include + provide a portable shim so the rest of the runtime can stay
// platform-agnostic. Used only by the optional poison-on-free path; if a
// platform has neither, the shim returns 0 (which makes __cajeta_poison_buffer
// silently skip — acceptable since poison-on-free is itself opt-in).
#if defined(__APPLE__)
#  include <malloc/malloc.h>
#  define cajeta_malloc_usable_size(p) malloc_size(p)
#elif defined(_WIN32)
#  include <malloc.h>
#  define cajeta_malloc_usable_size(p) _msize(p)
#elif defined(__GLIBC__) || defined(__linux__)
#  include <malloc.h>
#  define cajeta_malloc_usable_size(p) malloc_usable_size(p)
#else
#  define cajeta_malloc_usable_size(p) ((size_t) 0)
#endif

typedef void (*cajeta_ctor_fn)(void* self);

// ============================================================================
// Poison-on-free (CompilerModes.md § --poison-free).
//
// When enabled, every heap block is memset to a sentinel byte right before
// the actual free() runs. The pattern (0xDB) is unlikely to read as a valid
// pointer or sensible integer, so a use-after-free against poisoned memory
// either traps (deref of 0xDBDBDBDB...) or surfaces obviously wrong data.
//
// Flag wiring: __cajeta_set_poison_free(int) is called from the JIT-init
// hook (CajetaJit::compile) with the active CompilerFlags.poisonFree value,
// or — for binary compilation — from a global ctor emitted by the compiler.
// Default is off (0) so existing tests that don't go through the JIT hook
// keep prior behavior.
//
// Chunk-size source: malloc_usable_size(3) returns the actual allocated
// chunk (≥ requested size, may be larger due to malloc rounding). Poisoning
// the whole chunk is harmless — the over-fill stays inside the chunk and
// gets reclaimed at free time anyway.
// ============================================================================
static int __cajeta_poison_free_enabled = 0;

void __cajeta_set_poison_free(int enabled) {
    __cajeta_poison_free_enabled = enabled ? 1 : 0;
}

int __cajeta_get_poison_free(void) {
    return __cajeta_poison_free_enabled;
}

// Sentinel-fill a buffer with 0xDB up to its allocator-tracked chunk size.
// No-op when the flag is off or ptr is NULL. Kept as a separate symbol so
// tests can exercise the poison logic without dipping into use-after-free
// undefined behavior (the buffer is still valid after this call until free
// runs).
void __cajeta_poison_buffer(void* ptr) {
    if (!__cajeta_poison_free_enabled) return;
    if (!ptr) return;
    size_t n = cajeta_malloc_usable_size(ptr);
    if (n == 0) return;
    memset(ptr, 0xDB, n);
}

// ============================================================================
// Debug safepoints (debugger CP2+). When the compiler is run with
// --debug-info, statement-boundary codegen emits a call to
// __cajeta_dbg_safepoint(loc_id) before each statement. For CP2 this just
// counts hits so the TDD harness can verify emission/execution; CP3 adds
// breakpoint-arming + fiber park. As with the poison-free flag, the
// embedded-bitcode copy (called by JIT'd user code) and the native-object
// copy (callable from host C++ in tests) each keep their own static counter
// — read whichever copy you exercised.
// ============================================================================
static long __cajeta_dbg_safepoint_total = 0;

// Monotonic fiber-id source (CP3). Defined here so __cajeta_task_run (further
// down) can bump it at fiber creation; read back via dbg_id on the fiber.
long __cajeta_dbg_fiber_id_counter = 0;

// ── Debugger CP6f-2: live-fiber registry ──────────────────────────────────
// A snapshot of currently-live fibers so the DAP `threads`/fibers view can
// enumerate them: a fiber registers at __cajeta_task_run and unregisters when
// the carrier frees it (state == DONE). Stores opaque fiber handles; the
// per-fiber accessors further down cast them back to struct cajeta_fiber* so
// the host's native runtime copy can read a fiber the JIT copy registered.
//
// Guarded by its own mutex with a strict one-way nesting: the spawn path locks
// task→reg, while the carrier-free and host-enumeration paths lock reg only —
// nothing ever locks reg then task, so there's no deadlock against the task
// mutex. Enumeration happens on the debugger thread while a breakpoint is
// parked (the carrier holds no reg lock then), so it never blocks the program.
static pthread_mutex_t __cajeta_dbg_fiber_reg_mutex = PTHREAD_MUTEX_INITIALIZER;
static void** __cajeta_dbg_fiber_reg = NULL;
static int __cajeta_dbg_fiber_reg_count = 0;
static int __cajeta_dbg_fiber_reg_cap = 0;

void __cajeta_dbg_fiber_register(void* fiber) {
    if (!fiber) return;
    pthread_mutex_lock(&__cajeta_dbg_fiber_reg_mutex);
    if (__cajeta_dbg_fiber_reg_count == __cajeta_dbg_fiber_reg_cap) {
        int cap = __cajeta_dbg_fiber_reg_cap ? __cajeta_dbg_fiber_reg_cap * 2 : 16;
        void** grown = realloc(__cajeta_dbg_fiber_reg, (size_t) cap * sizeof(void*));
        if (!grown) { pthread_mutex_unlock(&__cajeta_dbg_fiber_reg_mutex); return; }
        __cajeta_dbg_fiber_reg = grown;
        __cajeta_dbg_fiber_reg_cap = cap;
    }
    __cajeta_dbg_fiber_reg[__cajeta_dbg_fiber_reg_count++] = fiber;
    pthread_mutex_unlock(&__cajeta_dbg_fiber_reg_mutex);
}

void __cajeta_dbg_fiber_unregister(void* fiber) {
    if (!fiber) return;
    pthread_mutex_lock(&__cajeta_dbg_fiber_reg_mutex);
    for (int i = 0; i < __cajeta_dbg_fiber_reg_count; i++) {
        if (__cajeta_dbg_fiber_reg[i] != fiber) continue;
        // Order-preserving removal: shift the tail down so the view stays in
        // stable spawn order across stops (no swap-with-last hole).
        for (int j = i + 1; j < __cajeta_dbg_fiber_reg_count; j++) {
            __cajeta_dbg_fiber_reg[j - 1] = __cajeta_dbg_fiber_reg[j];
        }
        __cajeta_dbg_fiber_reg_count--;
        break;
    }
    pthread_mutex_unlock(&__cajeta_dbg_fiber_reg_mutex);
}

// Number of live fibers (debugger thread reads this while the program is
// parked). Excludes the program/main thread, which the DAP layer reports as a
// synthetic id-0 thread.
int __cajeta_dbg_fiber_count(void) {
    pthread_mutex_lock(&__cajeta_dbg_fiber_reg_mutex);
    int n = __cajeta_dbg_fiber_reg_count;
    pthread_mutex_unlock(&__cajeta_dbg_fiber_reg_mutex);
    return n;
}

// The index-th live fiber handle (spawn order), or NULL if out of range.
void* __cajeta_dbg_fiber_at(int index) {
    pthread_mutex_lock(&__cajeta_dbg_fiber_reg_mutex);
    void* f = (index >= 0 && index < __cajeta_dbg_fiber_reg_count)
                  ? __cajeta_dbg_fiber_reg[index]
                  : NULL;
    pthread_mutex_unlock(&__cajeta_dbg_fiber_reg_mutex);
    return f;
}

// Test-only: drop all registry entries (does NOT free the fibers themselves).
void __cajeta_dbg_fiber_reg_reset(void) {
    pthread_mutex_lock(&__cajeta_dbg_fiber_reg_mutex);
    __cajeta_dbg_fiber_reg_count = 0;
    pthread_mutex_unlock(&__cajeta_dbg_fiber_reg_mutex);
}

// CP3: a settable safepoint handler. When the in-process debugger is attached
// it installs one (via the JIT symbol so the embedded-bitcode copy's pointer
// is set); the handler decides — based on the host-side armed set — whether to
// park the calling thread. NULL by default, so a debug-info build with no
// debugger attached just counts (CP2 behavior). The fiber id is resolved via
// __cajeta_dbg_current_fiber_id, which is defined later (after the
// __cajeta_current_fiber TLS); forward-declared here.
// CP5: per-fiber debug frame chain. When --debug-info is on, codegen emits a
// __cajeta_dbg_frame_enter at each method prologue, __cajeta_dbg_frame_leave on
// every return path, and __cajeta_dbg_local at each named local/parameter
// alloca. The chain (one node per live call frame, innermost at the head)
// lets the debugger walk the stack and read locals when a safepoint parks.
//
// Like scope_top/drop_top, the head lives per-fiber (a single __thread would
// alias across fiber switches on the same carrier); the selector
// __cajeta_dbg_top_ptr (defined near __cajeta_scope_top_ptr) picks the running
// fiber's slot or the main/program-thread TLS. The `name`/`type` strings are
// codegen-emitted constants (valid for the program's lifetime); `addr` is the
// local's slot alloca. CAVEAT: frame_leave fires only on normal return paths,
// not exception unwinding — a throw across a debug frame leaks its node (the
// breakpoint-inspect flow doesn't throw; revisit if stepping through throws).
#define CAJETA_DBG_MAX_LOCALS 64
struct cajeta_dbg_local {
    const char* name;
    const char* type;   // cajeta canonical type name (e.g. "int32", "demo.Foo")
    void* addr;          // the local's slot (primitives: holds the value;
                         // objects: holds the heap pointer)
    // CP7-1b: memory facets for ownership/allocation visualization. Two
    // orthogonal bytes mirroring cajeta::dbg::AllocClass / OwnershipRole
    // (see src/cajeta/dbg/MemoryFacets.h): alloc = where the value lives
    // (0 unknown / 1 stack / 2 heap / 3 shared), ownership = who is
    // responsible (0 unknown / 1 owner / 2 borrow / 3 moved-out). Carried
    // here as plain bytes so this C ABI stays decoupled from the C++ enum;
    // the host reads them back through the accessors below and maps them.
    uint8_t alloc;
    uint8_t ownership;
    // CP7-1c: the owner's drop-chain entry (a struct cajeta_drop_entry*, base
    // or debug shape — its `active` flag lives at the same offset in both), or
    // NULL for a non-owner. The host reads `active` LIVE at a stop to derive
    // lifetime state (active => about-to-drop, cleared => moved-out at runtime).
    // A raw void* so this struct stays above the cajeta_drop_entry definition.
    void* drop_entry;
};
struct cajeta_dbg_frame {
    const char* func;          // cajeta-mangled enclosing function name
    int32_t current_loc;       // loc_id of the last safepoint hit in this frame
    int nlocals;
    struct cajeta_dbg_local locals[CAJETA_DBG_MAX_LOCALS];
    struct cajeta_dbg_frame* prev;
};

// Selector for the current dbg frame chain head — mirrors __cajeta_scope_top_ptr
// (fiber vs main TLS). Defined further down where __cajeta_current_fiber is in
// scope; forward-declared here so the enter/leave/local helpers can use it.
struct cajeta_dbg_frame** __cajeta_dbg_top_ptr(void);

void __cajeta_dbg_frame_enter(const char* func) {
    struct cajeta_dbg_frame* f = malloc(sizeof(*f));
    if (!f) {
        fprintf(stderr, "cajeta: __cajeta_dbg_frame_enter malloc failed\n");
        abort();
    }
    f->func = func;
    f->current_loc = -1;
    f->nlocals = 0;
    struct cajeta_dbg_frame** top = __cajeta_dbg_top_ptr();
    f->prev = *top;
    *top = f;
}

void __cajeta_dbg_frame_leave(void) {
    struct cajeta_dbg_frame** top = __cajeta_dbg_top_ptr();
    struct cajeta_dbg_frame* f = *top;
    if (!f) return;
    *top = f->prev;
    free(f);
}

void __cajeta_dbg_local(const char* name, const char* type, void* addr,
                        uint8_t alloc, uint8_t ownership, void* drop_entry) {
    struct cajeta_dbg_frame** top = __cajeta_dbg_top_ptr();
    struct cajeta_dbg_frame* f = *top;
    if (!f || f->nlocals >= CAJETA_DBG_MAX_LOCALS) return;
    f->locals[f->nlocals].name = name;
    f->locals[f->nlocals].type = type;
    f->locals[f->nlocals].addr = addr;
    f->locals[f->nlocals].alloc = alloc;
    f->locals[f->nlocals].ownership = ownership;
    f->locals[f->nlocals].drop_entry = drop_entry;
    f->nlocals++;
}

// Stateless host-side accessors. The frame chain is built by the embedded
// bitcode copy of this runtime (JIT'd code calls frame_enter/local); the host
// reads it through the NATIVE copy. Pure pointer arithmetic on a passed-in
// void* means both copies resolve a chain node identically, so the host can
// dereference a pointer the JIT side produced. Used by DebugVars::walkFrames.
int __cajeta_dbg_frame_depth(void* top) {
    int n = 0;
    for (struct cajeta_dbg_frame* f = top; f; f = f->prev) n++;
    return n;
}
void* __cajeta_dbg_frame_prev(void* frame) {
    return frame ? ((struct cajeta_dbg_frame*) frame)->prev : NULL;
}
const char* __cajeta_dbg_frame_func(void* frame) {
    return frame ? ((struct cajeta_dbg_frame*) frame)->func : NULL;
}
int32_t __cajeta_dbg_frame_loc(void* frame) {
    return frame ? ((struct cajeta_dbg_frame*) frame)->current_loc : -1;
}
int __cajeta_dbg_frame_nlocals(void* frame) {
    return frame ? ((struct cajeta_dbg_frame*) frame)->nlocals : 0;
}
const char* __cajeta_dbg_local_name(void* frame, int i) {
    if (!frame) return NULL;
    struct cajeta_dbg_frame* f = frame;
    if (i < 0 || i >= f->nlocals) return NULL;
    return f->locals[i].name;
}
const char* __cajeta_dbg_local_type(void* frame, int i) {
    if (!frame) return NULL;
    struct cajeta_dbg_frame* f = frame;
    if (i < 0 || i >= f->nlocals) return NULL;
    return f->locals[i].type;
}
void* __cajeta_dbg_local_addr(void* frame, int i) {
    if (!frame) return NULL;
    struct cajeta_dbg_frame* f = frame;
    if (i < 0 || i >= f->nlocals) return NULL;
    return f->locals[i].addr;
}
// CP7-1b: the two memory facets. Out-of-range reads back 0 (== Unknown), the
// same neutral fallback codegen uses when a facet isn't statically known.
uint8_t __cajeta_dbg_local_alloc(void* frame, int i) {
    if (!frame) return 0;
    struct cajeta_dbg_frame* f = frame;
    if (i < 0 || i >= f->nlocals) return 0;
    return f->locals[i].alloc;
}
uint8_t __cajeta_dbg_local_ownership(void* frame, int i) {
    if (!frame) return 0;
    struct cajeta_dbg_frame* f = frame;
    if (i < 0 || i >= f->nlocals) return 0;
    return f->locals[i].ownership;
}

// CP5: the handler now also receives the current dbg frame-chain head so the
// host can walk frames + read locals without a TLS lookup (the chain lives in
// the bitcode copy's TLS, unreachable from the host's native copy).
typedef void (*cajeta_dbg_handler_fn)(int32_t loc_id, int fiber_id,
                                      void* frame_top);
static cajeta_dbg_handler_fn __cajeta_dbg_handler = NULL;
int __cajeta_dbg_current_fiber_id(void);

void __cajeta_dbg_set_safepoint_handler(cajeta_dbg_handler_fn fn) {
    __cajeta_dbg_handler = fn;
}

void __cajeta_dbg_safepoint(int32_t loc_id) {
    __cajeta_dbg_safepoint_total++;
    // Record the line we're at in the innermost frame so a multi-frame
    // stackTrace shows each frame at its current/call statement.
    struct cajeta_dbg_frame* top = *__cajeta_dbg_top_ptr();
    if (top) top->current_loc = loc_id;
    cajeta_dbg_handler_fn h = __cajeta_dbg_handler;
    if (h) h(loc_id, __cajeta_dbg_current_fiber_id(), top);
}

// CP6f-3: settable exception handler. When the in-process debugger attaches
// with exception breakpoints armed it installs one (via the JIT symbol, like
// the safepoint handler); __cajeta_throw calls it at the throw chokepoint —
// BEFORE the stack unwinds — so the throwing frame chain is still intact for
// inspection. NULL by default (throws proceed normally). The handler receives
// the thrown Throwable*, the current fiber id, and the dbg frame-chain head.
typedef void (*cajeta_dbg_exception_fn)(void* throwable, int fiber_id,
                                        void* frame_top);
// NOT static: __cajeta_throw (far away in this TU) reads this global. Under the
// JIT's partitioned/lazy materialization an `internal` global referenced across
// distant functions can end up duplicated (the setter writes one copy, the
// throw reads another) — observed as the exception handler never firing under
// `cajeta dap` while the adjacent safepoint handler worked. External linkage
// gives a single unified definition. (CP6f-3.)
cajeta_dbg_exception_fn __cajeta_dbg_exception_handler = NULL;

void __cajeta_dbg_set_exception_handler(cajeta_dbg_exception_fn fn) {
    __cajeta_dbg_exception_handler = fn;
}

long __cajeta_dbg_safepoint_count(void) {
    return __cajeta_dbg_safepoint_total;
}

void __cajeta_dbg_reset_safepoint_count(void) {
    __cajeta_dbg_safepoint_total = 0;
}

// ============================================================================
// Live-allocation set (FieldOwnership.md § Solution B).
//
// The auto field-drop scheme is "try to drop all fields; if the address is
// already gone, no-op." That requires a quick "is this address still live?"
// answer at every drop site. The live-set tracks every heap allocation we
// hand out; drop dispatchers atomically remove-and-claim, so the first call
// wins and subsequent calls (auto-drop and chain pop racing on the same
// aliased field, e.g. ArrayStream.data and ArrayList.data) no-op safely.
//
// Implementation: single global open-addressed hash table, fixed power-of-2
// capacity, pointer-shifted hash. Cross-fiber correctness needs the global
// (allocations may be freed on a different carrier than they were made on).
// Capacity is fixed; no resize — at 75% load we warn and start leaking the
// excess (correctness-preserving — leaked entries just mean future double-
// frees on those addresses won't be caught).
// ============================================================================
#define CAJETA_LIVE_SET_CAPACITY (1 << 16)   // 64K slots; 512KB total
#define CAJETA_LIVE_SET_LOAD_CAP ((CAJETA_LIVE_SET_CAPACITY * 3) / 4)
#define CAJETA_LIVE_SET_TOMBSTONE ((void*) 1)  // page 0 unmapped; safe sentinel

static void* __cajeta_live_set[CAJETA_LIVE_SET_CAPACITY];
static int __cajeta_live_set_count = 0;
static pthread_mutex_t __cajeta_live_set_mu = PTHREAD_MUTEX_INITIALIZER;

static void __cajeta_live_set_add_locked(void* p) {
    if (!p || p == CAJETA_LIVE_SET_TOMBSTONE) return;
    if (__cajeta_live_set_count >= CAJETA_LIVE_SET_LOAD_CAP) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr,
                "cajeta: live-allocation set reached load cap (%d / %d). "
                "Subsequent allocations won't be tracked; field auto-drop "
                "may double-free aliased addresses past this point. "
                "Raise CAJETA_LIVE_SET_CAPACITY in runtime/native/cajeta_runtime.c "
                "(see cajeta-docs/FieldOwnership.md).\n",
                __cajeta_live_set_count, CAJETA_LIVE_SET_CAPACITY);
            warned = 1;
        }
        return;
    }
    size_t mask = CAJETA_LIVE_SET_CAPACITY - 1;
    size_t bucket = ((uintptr_t) p >> 3) & mask;
    for (size_t i = 0; i < CAJETA_LIVE_SET_CAPACITY; i++) {
        size_t idx = (bucket + i) & mask;
        void* cur = __cajeta_live_set[idx];
        if (cur == NULL || cur == CAJETA_LIVE_SET_TOMBSTONE) {
            __cajeta_live_set[idx] = p;
            __cajeta_live_set_count++;
            return;
        }
        if (cur == p) return;  // already present (shouldn't happen — alloc gave a fresh address)
    }
}

static int __cajeta_live_set_remove_locked(void* p) {
    if (!p || p == CAJETA_LIVE_SET_TOMBSTONE) return 0;
    size_t mask = CAJETA_LIVE_SET_CAPACITY - 1;
    size_t bucket = ((uintptr_t) p >> 3) & mask;
    for (size_t i = 0; i < CAJETA_LIVE_SET_CAPACITY; i++) {
        size_t idx = (bucket + i) & mask;
        void* cur = __cajeta_live_set[idx];
        if (cur == NULL) return 0;  // empty slot — search ends
        if (cur == p) {
            __cajeta_live_set[idx] = CAJETA_LIVE_SET_TOMBSTONE;
            __cajeta_live_set_count--;
            return 1;
        }
    }
    return 0;
}

void __cajeta_live_set_add(void* p) {
    pthread_mutex_lock(&__cajeta_live_set_mu);
    __cajeta_live_set_add_locked(p);
    pthread_mutex_unlock(&__cajeta_live_set_mu);
}

// Returns 1 if the address was in the set and has been removed; 0 otherwise.
// This is the atomic "claim" used by drop dispatchers: only the first caller
// gets a 1 and is responsible for running the destructor + free.
int __cajeta_live_set_claim(void* p) {
    pthread_mutex_lock(&__cajeta_live_set_mu);
    int r = __cajeta_live_set_remove_locked(p);
    pthread_mutex_unlock(&__cajeta_live_set_mu);
    return r;
}

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
    __cajeta_live_set_add(buf);
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
    // Overflow guard: total = header_size + count*elem_size in uint64 would wrap
    // (e.g. `new int[-1]` arrives as count=0xFFFF...), and `calloc(1, total)`
    // performs no nmemb*size check, so a wrapped `total` under-allocates and the
    // count store + element writes overrun the heap. Reject before computing.
    if (elem_size != 0 && count > (UINT64_MAX - header_size) / elem_size) {
        fprintf(stderr, "cajeta: __cajeta_new_array_header overflow (header=%llu elem=%llu count=%llu)\n",
                (unsigned long long) header_size,
                (unsigned long long) elem_size,
                (unsigned long long) count);
        abort();
    }
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
    __cajeta_live_set_add(hdr);
    return hdr;
}

void* __cajeta_alloc(uint64_t size);  // defined below; used by __cajeta_args_make

// Materialize a cajeta `String[]` from C `argv` for a `main(String[] args)`
// entry point. The String struct's total size and field byte offsets, plus the
// String vtable, are passed in from the emit shim (computed via LLVM's
// DataLayout on the real class type) so nothing about the String ABI is
// hardcoded here. Returns a standard CajetaArray `{ i64 count, [count x ptr] }`
// of owned (mode=0) String instances, each holding a heap copy of an argv slot.
void* __cajeta_args_make(int64_t argc, char** argv,
                         void* string_vtable, int64_t str_size,
                         int64_t off_bytes, int64_t off_byte_len,
                         int64_t off_mode, int64_t off_cplen) {
    if (argc < 0) argc = 0;
    // cajeta `String[]` has array LLVM type `{ i64, [0 x %String] }`, so the
    // element STRIDE is the full String struct size — but each slot holds a
    // `String*` POINTER in its first 8 bytes (the codegen stores/loads a
    // pointer per element; see the aggregate-init lowering). So: allocate the
    // backing with `str_size` stride, then store one heap String* per slot.
    void* arr = __cajeta_new_array_header(8, (uint64_t) str_size, (uint64_t) argc);
    char* base = (char*) arr + 8;
    for (int64_t i = 0; i < argc; i++) {
        const char* s = (argv && argv[i]) ? argv[i] : "";
        int64_t len = (int64_t) strlen(s);
        // bytes payload: CajetaArray { i64 count=len, [len+1 x i8] } — the
        // trailing NUL keeps any legacy strlen reader happy (matches the
        // string-literal materialization in LiteralExpression.cpp).
        void* bytes = __cajeta_new_array_header(8, 1, (uint64_t) (len + 1));
        *((int64_t*) bytes) = len;                       // count excludes the NUL
        memcpy((char*) bytes + 8, s, (size_t) len + 1);  // copy incl. the NUL
        // Heap String instance (vtable is field 0, offset 0 by construction).
        void* str = __cajeta_alloc((uint64_t) str_size);
        *(void**)   ((char*) str)                = string_vtable;
        *(void**)   ((char*) str + off_bytes)    = bytes;
        *(int32_t*) ((char*) str + off_byte_len) = (int32_t) len;
        *(int32_t*) ((char*) str + off_mode)     = 0;    // owned: drop reclaims bytes
        *(int32_t*) ((char*) str + off_cplen)    = -1;   // codepoint length uncomputed
        // Store the pointer at the (str_size-strided) element slot.
        *(void**) (base + (size_t) i * (size_t) str_size) = str;
    }
    return arr;
}

// Idempotent — see FieldOwnership.md § Solution B. Auto field drop and the
// owning local's chain pop both call this for the same array address; the
// first one wins the live-set claim and actually frees, the second sees
// the address is gone and returns silently.
void __cajeta_free_array(void* ptr) {
    if (!ptr) return;
    if (!__cajeta_live_set_claim(ptr)) return;
    __cajeta_poison_buffer(ptr);
    free(ptr);
}

// Drop helper for OWNING views (Views.md § Construction).
// A view value is a pointer into the data region of a byte[] (the view-ctor
// codegen GEPs past the 8-byte array header). For an owning view, scope-exit
// must free the underlying array header, not the data pointer. This helper
// recovers the header by subtracting the header offset and then calls
// __cajeta_free_array. Kept as a dedicated symbol so the drop-fn pointer
// types match (void(*)(void*)) without callers needing to know the offset.
void __cajeta_view_drop_owned(void* data_ptr) {
    if (data_ptr == NULL) return;
    void* header = (void*) ((char*) data_ptr - 8);
    if (!__cajeta_live_set_claim(header)) return;
    __cajeta_poison_buffer(header);
    free(header);
}

// Materialize a heap-allocated T[] from a view's variable-size T[] field
// (S5b — Views.md § Variable-size fields). The view's bytes don't include
// an array header — they're just `count * elem_size` packed element bytes.
// This helper allocates a fresh array header + data block and memcpys the
// element bytes into it. Returns the header pointer (matches the layout
// __cajeta_new_array_header produces).
//
// Caller responsibility: free the returned pointer via the standard array
// drop path when the result goes out of scope. The drop entry for the
// caller's local is registered by LocalVariableDeclaration as usual.
void* __cajeta_array_view_to_owned(const void* data, int64_t count, int64_t elem_size) {
    if (count < 0) count = 0;
    if (elem_size <= 0) elem_size = 1;
    uint64_t header_size = 8;
    // Same overflow guard as __cajeta_new_array_header (count/elem are clamped
    // non-negative above but the product can still wrap uint64).
    if ((uint64_t) count > (UINT64_MAX - header_size) / (uint64_t) elem_size) {
        fprintf(stderr, "cajeta: __cajeta_array_view_to_owned overflow (count=%lld elem=%lld)\n",
                (long long) count, (long long) elem_size);
        abort();
    }
    uint64_t total = header_size + (uint64_t) count * (uint64_t) elem_size;
    void* hdr = calloc(1, (size_t) total);
    if (hdr == NULL) {
        fprintf(stderr, "cajeta: __cajeta_array_view_to_owned failed (count=%lld elem=%lld)\n",
                (long long) count, (long long) elem_size);
        abort();
    }
    *((int64_t*) hdr) = (int64_t) count;
    if (data != NULL && count > 0) {
        memcpy((char*) hdr + header_size, data, (size_t) count * (size_t) elem_size);
    }
    __cajeta_live_set_add(hdr);
    return hdr;
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
    __cajeta_live_set_add(p);
    return p;
}

// Mirror of __cajeta_free_array for non-array heap blocks. Kept as a
// separate symbol so the drop-fn function-pointer types match what the
// emitted IR uses for arrays (both are `void(*)(void*)`).
//
// Unconditional: this is the actual body-free that runs at the end of
// every class drop wrapper, after __cajeta_class_virtual_drop has
// already claimed the instance out of the live-set. Idempotency for
// class instances lives in virtual_drop's claim, not here.
void __cajeta_free(void* ptr) {
    if (!ptr) return;
    __cajeta_live_set_claim(ptr);  // remove if present; ignore result
    __cajeta_poison_buffer(ptr);
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

// The lock primitives live AFTER the fiber executor (further down in
// this file) so they can use the fiber struct + carrier state directly.
// Forward declarations are intentionally avoided — re-declaring `static
// __thread` storage with separate definitions is a footgun on some
// compilers, so the file is ordered: shared infrastructure first, then
// users of that infrastructure.

// --- Threading: stackful fiber executor (R3-B) ----------------------------
//
// ThreadModel.md async runtime: each spawn produces a fiber — a userspace
// task with its own stack (allocated separately, ~64KB). A single carrier
// OS thread runs many fibers cooperatively via ucontext.h. When a fiber
// calls await on a not-yet-done Task, it parks (saves its context, returns
// to the carrier); the carrier then picks another ready fiber. When any
// task completes, all parked fibers move back to the ready queue and
// re-check their await conditions. Polling-wake is inefficient (every
// completion wakes every parker) but correct; per-task wait queues land
// in a later iteration.
//
// The main thread is NOT a fiber. Its await OS-blocks on a condvar — the
// program's entry point can sit there waiting for the top-level spawned
// task to complete. This mirrors Java 21's "platform thread" vs "virtual
// thread" distinction.
//
// Why stackful (not stackless state machines)? Faster to ship — no async-
// fn codegen transformation — and removes function coloring (any function
// can call await, not just `async` ones). The per-fiber stack cost (~64KB)
// matches Java 21's virtual-thread model and is acceptable for v1. A
// stackless rewrite remains possible later if measured cost demands it.

// ucontext.h — glibc + macOS provide swapcontext / getcontext / makecontext
// for stackful coroutine context-switching. MinGW-w64 doesn't ship a
// ucontext.h (libucontext isn't packaged for mingw-w64), so on Windows we
// roll a minimal shim over the Win32 Fibers API (CreateFiber +
// SwitchToFiber + ConvertThreadToFiber), which is the canonical Windows
// equivalent for cooperative user-mode coroutines.
//
// The shim's ucontext_t struct keeps the uc_stack { ss_sp, ss_size } /
// uc_link fields the cajeta fiber init code reads, even though Windows
// manages the fiber stack internally — the fields are ignored at runtime.
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

typedef struct {
    LPVOID fiber;                         // Windows fiber handle
    void (*entry)(void);                  // makecontext-supplied entry
    struct { void* ss_sp; size_t ss_size; } uc_stack;
    void* uc_link;
} ucontext_t;

static VOID CALLBACK __cajeta_w32_fiber_trampoline(LPVOID param) {
    ucontext_t* uc = (ucontext_t*) param;
    if (uc && uc->entry) uc->entry();
    // The fiber function returned. Hand control to uc_link (the carrier that
    // dispatched us), mirroring ucontext's uc_link semantics.
    //
    // It MUST be uc_link, not GetCurrentFiber(): GetCurrentFiber() returns
    // THIS fiber, and SwitchToFiber(self) is undefined — in practice the
    // callback then returns and Windows terminates the carrier thread, so the
    // carrier dies after the first task finishes and any later task never runs
    // (its awaiter deadlocks). swapcontext/uc_link is the path glibc takes when
    // a makecontext fiber falls off its end.
    ucontext_t* link = uc ? (ucontext_t*) uc->uc_link : NULL;
    if (link && link->fiber) {
        SwitchToFiber(link->fiber);
    }
}

static inline int __cajeta_w32_getcontext(ucontext_t* uc) {
    if (uc) {
        uc->fiber = NULL;
        uc->entry = NULL;
        uc->uc_stack.ss_sp = NULL;
        uc->uc_stack.ss_size = 0;
        uc->uc_link = NULL;
    }
    return 0;
}

static inline void __cajeta_w32_makecontext(ucontext_t* uc, void (*func)(void), int argc) {
    (void) argc;  // cajeta always passes 0
    uc->entry = func;
    SIZE_T stack_size = uc->uc_stack.ss_size > 0
        ? (SIZE_T) uc->uc_stack.ss_size : 64 * 1024;
    uc->fiber = CreateFiber(stack_size, __cajeta_w32_fiber_trampoline, uc);
}

static inline int __cajeta_w32_swapcontext(ucontext_t* from, ucontext_t* to) {
    // First swap on the carrier thread must promote it to a fiber.
    if (!IsThreadAFiber()) {
        LPVOID cur = ConvertThreadToFiber(NULL);
        if (from) from->fiber = cur;
    } else if (from && !from->fiber) {
        from->fiber = GetCurrentFiber();
    }
    if (to && to->fiber) SwitchToFiber(to->fiber);
    return 0;
}

#define getcontext(uc)              __cajeta_w32_getcontext(uc)
#define makecontext(uc, func, argc) __cajeta_w32_makecontext(uc, func, argc)
#define swapcontext(from, to)       __cajeta_w32_swapcontext(from, to)

#else
#include <ucontext.h>
#endif
#include <string.h>

// Apple deprecated the ucontext.h family in 10.6 but ships no replacement for
// user-space context switching (its guidance -- GCD / pthreads -- can't express
// a cooperative fiber scheduler). ucontext is still the portable mechanism:
// glibc and the BSDs implement it, macOS implements it (just deprecated), and
// Windows is covered by the shim above. Route the three calls through wrappers
// so the residual macOS deprecation warning is silenced in exactly one place
// rather than at every call site; on every other platform these are zero-cost
// pass-throughs (and on Windows they expand to the __cajeta_w32_* shims).
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
static inline int __cajeta_getcontext(ucontext_t* uc) {
    return getcontext(uc);
}
static inline void __cajeta_makecontext(ucontext_t* uc, void (*func)(void), int argc) {
    makecontext(uc, func, argc);
}
static inline int __cajeta_swapcontext(ucontext_t* from, ucontext_t* to) {
    return swapcontext(from, to);
}
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif

// Per-fiber stack size. A fiber runs arbitrary Cajeta code, which routinely
// calls down into native C libraries (OpenSSL during a TLS handshake parses an
// X.509 chain, runs ECDSA P-256, decodes ASN.1 — a call tree that alone wants
// well over 64 KB of stack). The fiber stack is a plain `malloc` with NO guard
// page, so an overflow does not fault cleanly — it silently scribbles over
// adjacent heap and surfaces later as a SIGSEGV (full speed) or a wedged
// scheduler (under a debugger, where the heap layout differs). 64 KB was enough
// for the shallow channel/fan-out fibers the early tests exercised but far too
// little for a fiber that drives a real native library; size it like a
// conventional OS thread stack (1 MB) so any reasonable native call depth fits.
// Overridable via $CAJETA_FIBER_STACK_KB for pathological depths or tight
// memory budgets.
#define CAJETA_FIBER_STACK_SIZE (1024 * 1024)

// Resolve the per-fiber stack size once, honoring $CAJETA_FIBER_STACK_KB (a
// size in KiB) when set to a sane positive value, else CAJETA_FIBER_STACK_SIZE.
// Cached in a static so every fiber in the process gets the same size without
// re-parsing the environment. A clamp floors the override at 64 KiB so a
// mis-set tiny value can't reintroduce the overflow this default guards against.
static size_t __cajeta_fiber_stack_size(void) {
    static size_t cached = 0;
    if (cached == 0) {
        size_t sz = (size_t) CAJETA_FIBER_STACK_SIZE;
        const char* env = getenv("CAJETA_FIBER_STACK_KB");
        if (env && *env) {
            long kb = atol(env);
            if (kb >= 64) sz = (size_t) kb * 1024;
        }
        cached = sz;
    }
    return cached;
}

typedef void (*cajeta_task_trampoline_fn)(void* arg);

typedef enum {
    CAJETA_FIBER_READY,     // on the ready queue, will be resumed
    CAJETA_FIBER_RUNNING,   // on the carrier right now
    CAJETA_FIBER_PARKED,    // suspended at await, awaits global wake
    CAJETA_FIBER_DONE,      // trampoline returned; carrier will free
} cajeta_fiber_state;

// Forward declaration — scope_exit re-raises via __cajeta_throw, which
// is defined later in the file.
__attribute__((noreturn)) void __cajeta_throw(void* value);

// R5-A/R5-D: per-scope tracking of spawned child tasks. The scope's
// closing `}` calls __cajeta_scope_exit which waits for every registered
// task's done flag before returning, guaranteeing the doc's "control
// doesn't leave the block until every child has finished" property. R5-D
// adds the exception slot: after waiting, scope_exit walks each task's
// exception slot and re-raises the first one found.
struct cajeta_scope_entry {
    int32_t* done_addr;
    void** exception_addr;  // points to the Throwable* slot; NULL on success
    void** fiber_slot;      // points to Task's fiber-ptr slot; runtime fills it
};

struct cajeta_scope_frame {
    struct cajeta_scope_entry* entries;
    int count;
    int cap;
    struct cajeta_scope_frame* prev;
};

// Forward decls so cajeta_fiber can hold pointers to the drop and
// exception chain heads. Both chains are defined further down in this
// file (the file is ordered shared-infra first, then users).
struct cajeta_drop_entry;
struct cajeta_exception_frame;

struct cajeta_fiber {
    ucontext_t ctx;
    void* stack;
    cajeta_fiber_state state;
    cajeta_task_trampoline_fn trampoline;
    void* trampoline_arg;
    struct cajeta_fiber* next;
    // Per-fiber scope chain. A naïve `__thread` would alias across fiber
    // switches (the carrier hosts many fibers on the same OS thread), so
    // each fiber owns its own stack of scope frames. Updated by
    // scope_enter/exit when invoked from a fiber context.
    struct cajeta_scope_frame* scope_top;
    // Per-fiber drop chain head and exception chain head. Same rationale
    // as scope_top: a single OS-thread-level `__thread` would alias
    // across fiber switches on the same carrier, so each fiber owns its
    // own chain. The main thread has its own `__thread` slot below;
    // __cajeta_drop_top_ptr / __cajeta_exc_top_ptr pick the right one
    // based on whether __cajeta_current_fiber is set.
    struct cajeta_drop_entry* drop_top;
    struct cajeta_exception_frame* exc_top;
    // R5-C: cancellation marker. When non-NULL, the fiber's next
    // __cajeta_task_wait resume will throw this Throwable* instead of
    // returning normally. Set by __cajeta_fiber_cancel from scope's
    // first-throw escalation; cleared by the await re-raise path so
    // the same cancel doesn't fire twice on a fiber that survives.
    void* cancel_with;
    // C2: address of the Task's fiber-ptr slot (same as fiber_slot passed to
    // __cajeta_task_run). The carrier nulls *slot_ptr under __cajeta_task_mutex
    // before freeing the fiber, so a concurrent scope-cancel that reads the slot
    // (also under the mutex) never dereferences a freed fiber.
    void** slot_ptr;
    // Debugger CP3: stable per-fiber id for the DAP `threads`/`cajeta:fibers`
    // view and for stop events. Assigned from a monotonic counter at creation
    // (fibers get 1,2,3,...; the main thread reports id 0).
    int dbg_id;
    // Debugger CP5: per-fiber debug frame-chain head (locals capture).
    // Same aliasing rationale as scope_top/drop_top; selected by
    // __cajeta_dbg_top_ptr based on fiber-vs-main context.
    struct cajeta_dbg_frame* dbg_top;
    // Home carrier — the carrier that FIRST dispatched (started) this fiber.
    // -1 until then. Once a fiber has started, its saved `ucontext` (stack +
    // register state) is bound to that carrier; resuming it on a *different*
    // carrier is the unsolved cross-carrier handoff (corrupts on a multi-
    // carrier pool — see __cajeta_steal_one / __cajeta_publish_ready). So a
    // started fiber is pinned here: only its home carrier ever resumes it.
    // Fresh (not-yet-started) fibers have no saved context and may run on any
    // carrier — that's where the pool's parallelism comes from.
    int home_carrier;
};

static pthread_mutex_t __cajeta_task_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  __cajeta_task_queue_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  __cajeta_task_done_cond  = PTHREAD_COND_INITIALIZER;

// R8.2 — per-carrier Chase-Lev work-stealing deque, replacing the prior
// linked-list ready queue. Single-producer (the owning carrier — i.e. the
// thread that called push_bottom / pop_bottom) and multi-consumer (other
// carriers stealing from the top, see deque_steal). For v1 there's still
// only one carrier, so the atomic protocol is dormant under
// __cajeta_task_mutex; the structure is in place so R8.3 (multi-carrier
// pool) lifts the owner-side mutex with minimal churn while the steal
// path is already correct.
//
// Layout: a fixed-size circular slot array plus monotonically-growing
// `top` / `bottom` sequence numbers. Bottom is where the owner pushes /
// pops (LIFO); top is where stealers consume (FIFO from the deque's
// perspective). The slot index is `seq % CAJETA_DEQUE_CAP`. Capacity is
// generous for v1; overflow aborts. A growable variant lands when a
// workload actually needs it.
//
// References: Chase & Lev 2005 "Dynamic Circular Work-Stealing Deque"; the
// memory-ordering choices here mirror the cppmem-validated lowering in
// the standard libcds / Crossbeam-deque implementations.
#define CAJETA_DEQUE_CAP 2048

struct cajeta_carrier_deque {
    // top / bottom are accessed via __atomic_* builtins (no _Atomic
    // qualifier — those builtins take plain integer pointers).
    int64_t top;
    int64_t bottom;
    struct cajeta_fiber* slots[CAJETA_DEQUE_CAP];
};

static void __cajeta_deque_init(struct cajeta_carrier_deque* d) {
    __atomic_store_n(&d->top, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&d->bottom, 0, __ATOMIC_SEQ_CST);
}

// Owner-side push (LIFO end). Single-writer; under the v1 mutex it's
// uncontended. Capacity overflow aborts — v1 doesn't grow.
static void __cajeta_deque_push_bottom(struct cajeta_carrier_deque* d,
                                        struct cajeta_fiber* f) {
    int64_t b = __atomic_load_n(&d->bottom, __ATOMIC_RELAXED);
    int64_t t = __atomic_load_n(&d->top, __ATOMIC_ACQUIRE);
    if (b - t >= CAJETA_DEQUE_CAP) {
        fprintf(stderr, "cajeta: carrier deque overflow (%lld slots in use)\n",
                (long long) (b - t));
        abort();
    }
    d->slots[b % CAJETA_DEQUE_CAP] = f;
    // Release the slot write before publishing the new bottom — a future
    // stealer observing the new `bottom` must also see the slot's content.
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&d->bottom, b + 1, __ATOMIC_RELAXED);
}

// Owner-side pop (LIFO end). Returns NULL on empty. The Chase-Lev "last
// element" CAS is what makes the owner / stealer race tight even when
// only one fiber is left.
static struct cajeta_fiber* __cajeta_deque_pop_bottom(
        struct cajeta_carrier_deque* d) {
    int64_t b = __atomic_load_n(&d->bottom, __ATOMIC_RELAXED) - 1;
    __atomic_store_n(&d->bottom, b, __ATOMIC_RELAXED);
    // The SeqCst fence pairs with steal()'s SeqCst fence; without it,
    // pop_bottom and steal can both think they got the last element.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    int64_t t = __atomic_load_n(&d->top, __ATOMIC_RELAXED);
    if (t > b) {
        // Empty — restore bottom and bail.
        __atomic_store_n(&d->bottom, t, __ATOMIC_RELAXED);
        return NULL;
    }
    struct cajeta_fiber* f = d->slots[b % CAJETA_DEQUE_CAP];
    if (t < b) {
        // Multi-element case — uncontended.
        return f;
    }
    // t == b: last element. Race with steal() for it.
    int64_t expected = t;
    if (!__atomic_compare_exchange_n(&d->top, &expected, t + 1,
            /*weak=*/0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
        f = NULL;  // stealer won
    }
    __atomic_store_n(&d->bottom, t + 1, __ATOMIC_RELAXED);
    return f;
}

// Stealer-side pop (FIFO end). Returns NULL on empty or on a lost race
// (the owner / another stealer claimed the slot first). Unused by R8.2's
// single carrier — present so R8.3 lights it up without further deque
// surgery.
__attribute__((unused))
static struct cajeta_fiber* __cajeta_deque_steal(
        struct cajeta_carrier_deque* d) {
    int64_t t = __atomic_load_n(&d->top, __ATOMIC_ACQUIRE);
    // Pairs with pop_bottom's SeqCst fence.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    int64_t b = __atomic_load_n(&d->bottom, __ATOMIC_ACQUIRE);
    if (t >= b) return NULL;
    struct cajeta_fiber* f = d->slots[t % CAJETA_DEQUE_CAP];
    int64_t expected = t;
    if (!__atomic_compare_exchange_n(&d->top, &expected, t + 1,
            /*weak=*/0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
        return NULL;  // lost the race
    }
    return f;
}

// Snapshot the deque's size from the owner's perspective. Callers hold
// __cajeta_task_mutex so this is the only writer running; the load
// orderings can be relaxed.
static int64_t __cajeta_deque_size(struct cajeta_carrier_deque* d) {
    int64_t b = __atomic_load_n(&d->bottom, __ATOMIC_RELAXED);
    int64_t t = __atomic_load_n(&d->top, __ATOMIC_RELAXED);
    return b - t;
}
// Parked list: fibers blocked inside __cajeta_task_wait waiting for some
// task's done flag to flip. Wake-all-on-any-complete is the v1 strategy.
// Protected by __cajeta_task_mutex (the pool-level mutex).
static struct cajeta_fiber* __cajeta_parked_head = NULL;
static int __cajeta_task_workers_started = 0;
// Set by __cajeta_task_shutdown to signal the carrier loop to exit.
// The carrier's pthread_cond_wait predicate checks this alongside
// the ready-queue head so a shutdown request reliably unblocks it.
static int __cajeta_task_shutdown_requested = 0;

// R9.1 timer state (declared here so __cajeta_task_shutdown — which lives
// above the timer implementation block — can join the timer thread on
// teardown). The struct cajeta_timer_entry definition + the function
// bodies live further down alongside __cajeta_task_wait_timeout.
struct cajeta_timer_entry;
static struct cajeta_timer_entry* __cajeta_timer_head = NULL;
static pthread_cond_t __cajeta_timer_cond = PTHREAD_COND_INITIALIZER;
static pthread_t __cajeta_timer_thread;
static int __cajeta_timer_started = 0;
static int __cajeta_timer_shutdown_requested = 0;

// R9.4 reactor state (declared above task_shutdown for the same reason as
// the timer state). The waiter struct + reactor loop body live below
// alongside __cajeta_io_wait.
struct cajeta_io_waiter;
static struct cajeta_io_waiter* __cajeta_reactor_waiters = NULL;
static pthread_t __cajeta_reactor_thread;
static int __cajeta_reactor_started = 0;
static int __cajeta_reactor_shutdown_requested = 0;
#if defined(__linux__)
static int __cajeta_reactor_epfd = -1;
#endif

// NET-3.2 — net-reactor lifecycle teardown hook (defined in
// cajeta_net_reactor_lifecycle.c, #included at the bottom of this TU). Forward-
// declared here so __cajeta_task_shutdown can drain + close the net reactor's
// own lifecycle state (the `started` latch, the live-registration balance, the
// shutdown wake pipe) once the carriers + R9.4 reactor thread are joined. It is
// idempotent and a cheap no-op when no awaitable net op ever ran.
int32_t __cajeta_net_reactor_shutdown(void);

// R8.3 — multi-carrier pool, flag-gated N=1.
//
// Each carrier owns one Chase-Lev deque + a deque_mutex that protects
// non-steal accesses to it (push_bottom / pop_bottom). The deque's steal
// path stays lock-free, so other carriers can steal from this carrier's
// top without taking deque_mutex — only the owner-side ops (push_bottom,
// pop_bottom) serialize on it. Cross-thread pushes (main-thread spawn,
// lock_release waking a waiter, etc.) take the target carrier's deque_mutex
// — Chase-Lev's single-producer contract holds within the mutex.
//
// Pool-level coordination — sleep/wake of idle carriers, shutdown,
// parked-list bookkeeping — runs under __cajeta_task_mutex
// (pre-existing). One shared cond, __cajeta_task_queue_cond, that idle
// carriers cond_wait on; pushers signal one when sleeping_count > 0
// (broadcast on shutdown).
//
// Carrier count is read from $CAJETA_CARRIERS at the first
// __cajeta_task_run, clamped to [1, CAJETA_MAX_CARRIERS]. Default is
// min(_SC_NPROCESSORS_ONLN, CAJETA_DEFAULT_CARRIERS_CAP) — multi-carrier
// by default so spawned tasks actually run in parallel. Single-carrier
// behaviour the prior releases had is opt-in via CAJETA_CARRIERS=1
// (still the canonical knob for deterministic-order debug runs).
#define CAJETA_MAX_CARRIERS 16
// Default-cap so a 64-core box doesn't spin up 16 carriers on a
// program that has no work for them. Empirically picked at 4 — gives
// real parallelism for the typical channel/fan-out workloads in tests
// and stays comfortably inside CAJETA_MAX_CARRIERS' upper bound. Users
// who want more can set CAJETA_CARRIERS=N explicitly.
#define CAJETA_DEFAULT_CARRIERS_CAP 4

struct cajeta_carrier {
    pthread_t thread;
    int carrier_id;
    pthread_mutex_t deque_mutex;
    struct cajeta_carrier_deque deque;
};

static struct cajeta_carrier __cajeta_carriers[CAJETA_MAX_CARRIERS];
static int __cajeta_carrier_count = 0;
// Number of carriers currently parked in cond_wait. Pushers consult
// this to decide whether to signal — non-zero means a signal will
// land on a sleeper rather than burning cycles on a busy one.
static int __cajeta_sleeping_count = 0;

// Per-thread pointer to the running carrier struct. NULL on the main
// thread / any non-carrier thread. Used by __cajeta_publish_ready to
// route new work onto the spawning carrier's own deque (locality —
// the carrier that produced the work is likely to consume it next).
static __thread struct cajeta_carrier* __cajeta_my_carrier = NULL;

// Per-OS-thread state. Only the carrier thread will ever have a non-null
// __cajeta_current_fiber — the main thread sees it null and falls through
// to the cond_wait path in __cajeta_task_wait.
static __thread struct cajeta_fiber* __cajeta_current_fiber = NULL;
static __thread ucontext_t __cajeta_carrier_ctx;

// Scope chain for code running outside a fiber (the main entry point, or
// any pthread that isn't a carrier-hosted fiber). Fibers carry their own
// scope chain in `cajeta_fiber::scope_top`; the helper below picks the
// right one based on current context.
static __thread struct cajeta_scope_frame* __cajeta_main_scope_top = NULL;

// Debugger CP5: main/program-thread slot for the dbg frame chain (the JIT
// entry runs on a plain bg thread, not a carrier fiber, so it uses this).
static __thread struct cajeta_dbg_frame* __cajeta_main_dbg_top = NULL;

// Returns a pointer to the current scope_top slot — either the running
// fiber's slot or the main thread's TLS slot. Callers read or write
// through this pointer, so push/pop works uniformly regardless of
// fiber vs main context.
static struct cajeta_scope_frame** __cajeta_scope_top_ptr(void) {
    if (__cajeta_current_fiber) {
        return &__cajeta_current_fiber->scope_top;
    }
    return &__cajeta_main_scope_top;
}

// Debugger CP5: selector for the dbg frame-chain head, mirroring
// __cajeta_scope_top_ptr. Forward-declared up in the safepoint section.
struct cajeta_dbg_frame** __cajeta_dbg_top_ptr(void) {
    if (__cajeta_current_fiber) {
        return &__cajeta_current_fiber->dbg_top;
    }
    return &__cajeta_main_dbg_top;
}

// Debugger CP3: id of the fiber running on this carrier thread, or 0 when not
// in a fiber (the main thread / program entry). Forward-declared up in the
// debug-safepoint section; defined here where __cajeta_current_fiber is in
// scope.
int __cajeta_dbg_current_fiber_id(void) {
    return __cajeta_current_fiber ? __cajeta_current_fiber->dbg_id : 0;
}

// Debugger CP6f-2: stateless per-fiber accessors. Like the frame-chain
// accessors above, these cast an opaque handle (from __cajeta_dbg_fiber_at)
// back to the fiber struct so the host's NATIVE runtime copy can read a fiber
// the JIT copy registered (both copies lay the struct out identically).
// dbg_id is the stable per-fiber id; frame_top is the head of that fiber's
// debug frame chain (feed it to DebugVars::walkFrames); state is the
// cajeta_fiber_state enum value.
long __cajeta_dbg_fiber_id_of(void* fiber) {
    return fiber ? (long) ((struct cajeta_fiber*) fiber)->dbg_id : 0;
}

void* __cajeta_dbg_fiber_frame_top(void* fiber) {
    return fiber ? ((struct cajeta_fiber*) fiber)->dbg_top : NULL;
}

int __cajeta_dbg_fiber_state(void* fiber) {
    return fiber ? (int) ((struct cajeta_fiber*) fiber)->state : -1;
}

// Fiber entry trampoline — invoked by makecontext on first resume. Runs
// the user trampoline (which calls the async fn + signals done), then
// explicitly swaps back to the running carrier's TLS slot.
//
// R8.3 — explicit swap-back instead of relying on uc_link. uc_link is a
// pointer baked into the ucontext at makecontext time, and on glibc
// `&__cajeta_carrier_ctx` resolves to the carrier-thread that
// FIRST DISPATCHED the fiber. After a steal, a different carrier owns
// the fiber but uc_link still points at the original carrier's TLS slot;
// a fall-off-the-end then jumps into the wrong saved context. The
// explicit swapcontext here evaluates `&__cajeta_carrier_ctx` in the
// CURRENTLY running carrier's TLS, so it's always the right address.
static void __cajeta_fiber_entry(void) {
    struct cajeta_fiber* f = __cajeta_current_fiber;
    f->trampoline(f->trampoline_arg);
    f->state = CAJETA_FIBER_DONE;
    __cajeta_swapcontext(&f->ctx, &__cajeta_carrier_ctx);
}

// R8.3 — publish a ready fiber onto one of the pool's deques. Routes to
// the current carrier's deque when the caller is running on one (locality);
// otherwise carrier 0 (main-thread spawn, lock-release on the main thread,
// etc.). Takes the target carrier's deque_mutex around the push so
// Chase-Lev's single-producer invariant holds, then signals one sleeper on
// the pool condvar if any carrier is parked.
//
// Caller must NOT hold __cajeta_task_mutex; this function takes it
// internally for the sleeper-signal step. (Holding it would deadlock
// against this routine's own attempt to acquire it.)
static void __cajeta_publish_ready(struct cajeta_fiber* f) {
    f->state = CAJETA_FIBER_READY;
    f->next = NULL;
    // A fiber that has already started is pinned to its home carrier — its
    // saved ucontext can only be resumed there. Route it home regardless of
    // who is waking it. A fresh fiber (home_carrier < 0) has no saved context
    // yet, so route it to the waker's own carrier for locality (it may still be
    // stolen by an idle peer — safe, since it makecontext's on the stealer).
    struct cajeta_carrier* target;
    if (f->home_carrier >= 0) {
        target = &__cajeta_carriers[f->home_carrier];
    } else {
        target = __cajeta_my_carrier ? __cajeta_my_carrier
                                     : &__cajeta_carriers[0];
    }
    pthread_mutex_lock(&target->deque_mutex);
    __cajeta_deque_push_bottom(&target->deque, f);
    pthread_mutex_unlock(&target->deque_mutex);
    pthread_mutex_lock(&__cajeta_task_mutex);
    if (__cajeta_sleeping_count > 0) {
        pthread_cond_signal(&__cajeta_task_queue_cond);
    }
    pthread_mutex_unlock(&__cajeta_task_mutex);
}

// Detach every parked fiber and return them as a NULL-terminated list.
// Caller holds __cajeta_task_mutex. The caller is responsible for
// publishing each one onto a carrier's deque AFTER releasing the pool
// mutex — publishing under the pool mutex would invert the lock order
// publish_ready uses (deque → pool) and deadlock with concurrent pushes.
static struct cajeta_fiber* __cajeta_drain_parked_locked(void) {
    struct cajeta_fiber* drained = __cajeta_parked_head;
    __cajeta_parked_head = NULL;
    return drained;
}

// Total work across every carrier's deque. Lock-free — reads atomic
// top/bottom on each deque without taking deque mutexes. Used by carriers
// pre-sleep to avoid going to cond_wait when there's still work somewhere
// in the pool (the race-fix for the small window between a steal-scan
// finishing empty and the carrier committing to sleep).
static int64_t __cajeta_pool_total_work(void) {
    int64_t total = 0;
    for (int i = 0; i < __cajeta_carrier_count; ++i) {
        total += __cajeta_deque_size(&__cajeta_carriers[i].deque);
    }
    return total;
}

// Steal a ready fiber from a peer carrier's deque top. Round-robin starting
// after `self` to spread the steal load. NULL if every peer's deque is
// empty or every steal attempt loses the CAS race.
static struct cajeta_fiber* __cajeta_steal_one(struct cajeta_carrier* self) {
    int n = __cajeta_carrier_count;
    if (n <= 1) return NULL;
    int start = self ? self->carrier_id + 1 : 0;
    for (int step = 0; step < n; ++step) {
        int idx = (start + step) % n;
        if (&__cajeta_carriers[idx] == self) continue;
        struct cajeta_fiber* f = __cajeta_deque_steal(&__cajeta_carriers[idx].deque);
        if (!f) continue;
        // Only fresh fibers (no home yet) or fibers already homed to us may run
        // here. A started fiber pinned to another carrier carries a live
        // ucontext bound to that carrier — resuming it on `self` is the
        // cross-carrier handoff that corrupts. Hand it back to its home deque
        // (waking that carrier if it's parked) and keep scanning.
        if (f->home_carrier >= 0 && f->home_carrier != self->carrier_id) {
            struct cajeta_carrier* home = &__cajeta_carriers[f->home_carrier];
            pthread_mutex_lock(&home->deque_mutex);
            __cajeta_deque_push_bottom(&home->deque, f);
            pthread_mutex_unlock(&home->deque_mutex);
            pthread_mutex_lock(&__cajeta_task_mutex);
            if (__cajeta_sleeping_count > 0) {
                pthread_cond_broadcast(&__cajeta_task_queue_cond);
            }
            pthread_mutex_unlock(&__cajeta_task_mutex);
            continue;
        }
        return f;
    }
    return NULL;
}

// Park the running fiber. Called by __cajeta_task_wait when inside a fiber
// and the awaited task isn't done. Adds the fiber to parked_head, then
// swaps back to the carrier — the swap returns into this function only
// after the fiber gets woken and the carrier dispatches it again.
static void __cajeta_fiber_park(void) {
    struct cajeta_fiber* f = __cajeta_current_fiber;
    pthread_mutex_lock(&__cajeta_task_mutex);
    f->state = CAJETA_FIBER_PARKED;
    f->next = __cajeta_parked_head;
    __cajeta_parked_head = f;
    pthread_mutex_unlock(&__cajeta_task_mutex);
    __cajeta_swapcontext(&f->ctx, &__cajeta_carrier_ctx);
}

// Park variant for callers that ALREADY hold __cajeta_task_mutex. Enqueues
// the fiber on parked_head and releases the mutex BEFORE swapping to the
// carrier. The point is atomicity: a caller rechecks its wake condition
// (done flag, deadline, I/O registration) under the same mutex immediately
// before calling this, so the fiber is committed to parked_head before any
// concurrent waker — task_complete on another carrier, the reactor thread,
// the timer thread — can acquire the mutex and drain/remove it. Without
// this, the unlocked-check-then-separately-lock-and-park sequence has a
// lost-wakeup window: the waker fires between the check and the enqueue,
// finds nothing parked, and the fiber then parks forever. (Single-carrier
// runs never hit it — everything is cooperative on one OS thread — which is
// why the await/deadline/io tests only hang under the multi-carrier pool.)
// Home-carrier pinning makes the post-unlock pre-swap window safe: only this
// fiber's home carrier resumes it, and that is the very thread executing the
// swap, so it cannot be re-dispatched until the swap has saved its context.
static void __cajeta_fiber_park_locked(void) {
    struct cajeta_fiber* f = __cajeta_current_fiber;
    f->state = CAJETA_FIBER_PARKED;
    f->next = __cajeta_parked_head;
    __cajeta_parked_head = f;
    pthread_mutex_unlock(&__cajeta_task_mutex);
    __cajeta_swapcontext(&f->ctx, &__cajeta_carrier_ctx);
}

// Carrier loop. Pop own deque (LIFO, cache-warm); on empty, try stealing
// from peer carriers (lock-free); on universally-empty, sleep on the pool
// condvar until a pusher signals. R8.3 — the carrier struct comes in via
// the pthread_create arg so each carrier knows its own deque, mutex, and
// id without TLS-init plumbing.
static void* __cajeta_carrier_loop(void* arg) {
    struct cajeta_carrier* self = (struct cajeta_carrier*) arg;
    __cajeta_my_carrier = self;
    for (;;) {
        // Owner-side pop on self's deque. Under self->deque_mutex so a
        // concurrent cross-thread push (publish_ready from another
        // carrier or main) can't race with this pop.
        pthread_mutex_lock(&self->deque_mutex);
        struct cajeta_fiber* f = __cajeta_deque_pop_bottom(&self->deque);
        pthread_mutex_unlock(&self->deque_mutex);

        // Empty own deque — try stealing from peers (lock-free).
        if (!f) {
            f = __cajeta_steal_one(self);
        }

        if (!f) {
            // Pool-wide empty. Decide whether to exit, retry, or wait — all
            // off a SINGLE pool_total_work() read taken under the pool mutex.
            //
            // The earlier form called pool_total_work() twice (once for the
            // shutdown-exit check, once for the retry check). When work
            // transiently read >0 then 0 across those two calls, a carrier
            // that had ALREADY observed shutdown_requested fell through to
            // cond_wait — but the shutdown broadcast is one-shot and had
            // already fired, so that carrier never woke and __cajeta_task_
            // shutdown's join() wedged forever. Reading work once closes the
            // TOCTOU; checking shutdown FIRST guarantees a shutting-down
            // carrier never blocks on the one-shot condvar (it exits when the
            // pool is drained, else loops to drain reachable work). The wait
            // is also bounded as a final backstop against any missed wake on
            // the work path — the broadcast/signal still wakes us immediately
            // in the common case; the timeout only matters if a wake is lost.
            pthread_mutex_lock(&__cajeta_task_mutex);
            int64_t work = __cajeta_pool_total_work();
            if (__cajeta_task_shutdown_requested) {
                pthread_mutex_unlock(&__cajeta_task_mutex);
                if (work == 0) {
                    return NULL;
                }
                continue;  // drain reachable work; never cond_wait at shutdown
            }
            if (work > 0) {
                pthread_mutex_unlock(&__cajeta_task_mutex);
                continue;
            }
            __cajeta_sleeping_count++;
            struct timespec __wait_ts;
            clock_gettime(CLOCK_REALTIME, &__wait_ts);
            __wait_ts.tv_nsec += 50 * 1000 * 1000;  // 50ms backstop
            if (__wait_ts.tv_nsec >= 1000000000L) {
                __wait_ts.tv_nsec -= 1000000000L;
                __wait_ts.tv_sec += 1;
            }
            pthread_cond_timedwait(&__cajeta_task_queue_cond, &__cajeta_task_mutex, &__wait_ts);
            __cajeta_sleeping_count--;
            pthread_mutex_unlock(&__cajeta_task_mutex);
            continue;
        }
        f->next = NULL;

        __cajeta_current_fiber = f;
        f->state = CAJETA_FIBER_RUNNING;
        if (!f->stack) {
            // First-resume init: allocate the stack and prime the context
            // to dispatch to __cajeta_fiber_entry. uc_link points back at
            // this thread's carrier_ctx slot — single carrier per thread,
            // so the address stays stable across the fiber's lifetime.
            // For multi-carrier work-stealing (R8.3+), a fiber CAN land
            // on a different carrier than originally allocated; uc_link
            // would then need updating before each swap, but the obvious
            // `f->ctx.uc_link = &__cajeta_carrier_ctx` immediately
            // before swapcontext corrupts the saved trampoline state on
            // glibc (probably because makecontext snapshots uc_link
            // arch-dependently into the synthesized frame). v1 punts:
            // stealing is disabled below until the cross-carrier
            // uc_link handoff is solved at a deeper layer.
            size_t stack_size = __cajeta_fiber_stack_size();
            f->stack = malloc(stack_size);
            if (!f->stack) {
                fprintf(stderr, "cajeta: fiber stack malloc failed\n");
                abort();
            }
            // First dispatch: this carrier becomes the fiber's home. From here
            // on the fiber's saved ucontext is bound to this carrier, so only
            // this carrier may resume it (steal/publish honor home_carrier).
            f->home_carrier = self->carrier_id;
            __cajeta_getcontext(&f->ctx);
            f->ctx.uc_stack.ss_sp = f->stack;
            f->ctx.uc_stack.ss_size = stack_size;
            f->ctx.uc_link = &__cajeta_carrier_ctx;
            __cajeta_makecontext(&f->ctx, __cajeta_fiber_entry, 0);
        }
        __cajeta_swapcontext(&__cajeta_carrier_ctx, &f->ctx);
        __cajeta_current_fiber = NULL;
        if (f->state == CAJETA_FIBER_DONE) {
            // C2: null the Task's fiber slot before freeing, under the task mutex,
            // so a concurrent scope-cancel reading the slot (also under the mutex)
            // sees NULL instead of a dangling fiber pointer.
            pthread_mutex_lock(&__cajeta_task_mutex);
            if (f->slot_ptr) *f->slot_ptr = NULL;
            pthread_mutex_unlock(&__cajeta_task_mutex);
            // Debugger CP6f-2: drop from the live-fiber registry before freeing
            // so the fibers view never hands the host a dangling handle.
            __cajeta_dbg_fiber_unregister(f);
#if defined(_WIN32)
            // H19: on Windows the fiber is a Win32 fiber object (CreateFiber); the
            // free() below reclaims the struct but not the OS fiber. Delete it
            // (safe here — the fiber has returned to the carrier, it isn't current)
            // to avoid leaking one OS fiber + its stack per completed task.
            if (f->ctx.fiber) DeleteFiber(f->ctx.fiber);
#endif
            free(f->stack);
            free(f);
        }
        // Parked fibers stay on __cajeta_parked_head awaiting a wake.
    }
    return NULL;
}

// Signal the carrier thread to exit and join it. Used by JIT-mode test
// teardown so test 1's carrier doesn't survive into test 2 (each
// __cajeta_task_run lazy-starts a fresh carrier bound to its module's
// statics; without shutdown, test 1's carrier is left waiting on a
// condvar whose backing memory the JIT will recycle or unmap, and the
// next process-wide signal — typically test 2's cond_broadcast on the
// remapped condvar address — wakes that orphaned carrier into JIT code
// that no longer exists). Safe to call when no carrier was started:
// the flag check makes it a no-op.
void __cajeta_task_shutdown(void) {
    pthread_mutex_lock(&__cajeta_task_mutex);
    if (!__cajeta_task_workers_started) {
        pthread_mutex_unlock(&__cajeta_task_mutex);
        return;
    }
    __cajeta_task_shutdown_requested = 1;
    pthread_cond_broadcast(&__cajeta_task_queue_cond);
    int n = __cajeta_carrier_count;
    // R9.1 — if the timer thread was lazy-started, signal it to exit too.
    // Same JIT-survives-across-tests rationale as carrier shutdown: leaving
    // it running would let it observe (and signal on) a recycled condvar
    // address in the next test.
    int timer_was_started = __cajeta_timer_started;
    if (timer_was_started) {
        __cajeta_timer_shutdown_requested = 1;
        pthread_cond_signal(&__cajeta_timer_cond);
    }
    // R9.4 — same for the I/O reactor. It uses epoll_wait with a 1s
    // poll timeout, so the shutdown flag is observed within ~1s without
    // any explicit wake. Closing the epfd from outside would race the
    // reactor's epoll_wait return, so we let the poll timeout do it.
    int reactor_was_started = __cajeta_reactor_started;
    if (reactor_was_started) {
        __cajeta_reactor_shutdown_requested = 1;
    }
    pthread_mutex_unlock(&__cajeta_task_mutex);
    for (int i = 0; i < n; ++i) {
        pthread_join(__cajeta_carriers[i].thread, NULL);
    }
    if (timer_was_started) {
        pthread_join(__cajeta_timer_thread, NULL);
    }
    if (reactor_was_started) {
        pthread_join(__cajeta_reactor_thread, NULL);
    }
    // Reset state so a subsequent __cajeta_task_run (e.g. the next test's
    // first spawn) starts a fresh pool with clean queues. The deque per
    // carrier is re-initialized on the next workers_started=1 transition
    // in __cajeta_task_run.
    pthread_mutex_lock(&__cajeta_task_mutex);
    // H6: free any fibers still parked (one never woken) and null the head.
    // Otherwise their structs + 64KB stacks leak, and worse, a stale parked
    // fiber surviving into the next run would be re-readied by that run's first
    // task_complete and swapcontext'd into a recycled/unmapped JIT context.
    // (Runnable fibers live in the per-carrier deques under main's work-stealing
    // scheduler — there is no global ready queue to drain here.)
    for (struct cajeta_fiber* f = __cajeta_parked_head; f; ) {
        struct cajeta_fiber* nx = f->next;
        if (f->slot_ptr) *f->slot_ptr = NULL;
        free(f->stack); free(f); f = nx;
    }
    __cajeta_parked_head = NULL;
    __cajeta_task_shutdown_requested = 0;
    __cajeta_task_workers_started = 0;
    for (int i = 0; i < n; ++i) {
        pthread_mutex_destroy(&__cajeta_carriers[i].deque_mutex);
    }
    __cajeta_carrier_count = 0;
    if (timer_was_started) {
        __cajeta_timer_started = 0;
        __cajeta_timer_shutdown_requested = 0;
        __cajeta_timer_head = NULL;
    }
    if (reactor_was_started) {
        __cajeta_reactor_started = 0;
        __cajeta_reactor_shutdown_requested = 0;
        __cajeta_reactor_waiters = NULL;
#if defined(__linux__)
        if (__cajeta_reactor_epfd >= 0) {
            close(__cajeta_reactor_epfd);
            __cajeta_reactor_epfd = -1;
        }
#endif
    }
    pthread_mutex_unlock(&__cajeta_task_mutex);

    // NET-3.2 — tear down the net-reactor lifecycle (separate from the R9.4
    // engine above): wake any portable-path waiter, drain the live-registration
    // balance, reset the lazy-init latch, and close the shutdown wake pipe. Done
    // OUTSIDE __cajeta_task_mutex — it takes its own dedicated lifecycle mutex,
    // and keeping the two lock domains disjoint avoids any ordering coupling.
    // Idempotent + a no-op when no awaitable net op ever initialized it.
    __cajeta_net_reactor_shutdown();
}

// Enqueue a trampoline-arg pair as a fresh fiber. The actual stack +
// ucontext init is deferred to the carrier's first dispatch so cancelled-
// before-resumed spawns don't pay the stack-alloc cost. Lazy carrier
// thread start mirrors R2: programs that never spawn don't pay for a
// pthread_create. `fiber_slot` (R5-C) is the address of the Task's
// FIBER_FIELD slot — we write the freshly-allocated fiber's pointer
// there so scope can find the fiber by walking its registered task list.
void __cajeta_task_run(void* arg, cajeta_task_trampoline_fn trampoline,
                       void** fiber_slot) {
    struct cajeta_fiber* f = malloc(sizeof(*f));
    if (!f) {
        fprintf(stderr, "cajeta: __cajeta_task_run fiber malloc failed\n");
        abort();
    }
    f->ctx = (ucontext_t) {0};
    f->stack = NULL;
    f->state = CAJETA_FIBER_READY;
    f->trampoline = trampoline;
    f->trampoline_arg = arg;
    f->next = NULL;
    f->scope_top = NULL;
    f->drop_top = NULL;
    f->exc_top = NULL;
    f->cancel_with = NULL;
    f->slot_ptr = fiber_slot;   // C2: so the carrier can null it before free
    f->dbg_top = NULL;
    f->home_carrier = -1;   // assigned on first dispatch (see carrier_loop)
    if (fiber_slot) *fiber_slot = f;

    pthread_mutex_lock(&__cajeta_task_mutex);
    // Debugger CP3/CP6f-2: assign a stable id (fibers get 1,2,3,...; the
    // program/main thread reports id 0) and add to the live-fiber registry so
    // the DAP fibers view can enumerate it. (CP3 declared dbg_id but never
    // assigned it, so every fiber reported 0 — fixed here.) register() locks
    // the reg mutex INSIDE the task mutex; nothing locks the other order.
    f->dbg_id = (int) ++__cajeta_dbg_fiber_id_counter;
    __cajeta_dbg_fiber_register(f);
    if (!__cajeta_task_workers_started) {
        __cajeta_task_workers_started = 1;
        // Carrier count from $CAJETA_CARRIERS if set; otherwise default
        // to min(_SC_NPROCESSORS_ONLN, CAJETA_DEFAULT_CARRIERS_CAP) so
        // spawned tasks get real parallelism out of the box. Clamped to
        // [1, CAJETA_MAX_CARRIERS]. Read once at first spawn — the pool
        // is fixed-size for the lifetime of this scheduler instance.
        int n;
        const char* env = getenv("CAJETA_CARRIERS");
        if (env && *env) {
            int parsed = atoi(env);
            n = (parsed >= 1) ? parsed : 1;
        } else {
#if defined(_WIN32)
            // sysconf/_SC_NPROCESSORS_ONLN is POSIX; on Windows ask the Win32
            // API (windows.h is included at file scope for the fiber/lock paths).
            SYSTEM_INFO cpu_si;
            GetSystemInfo(&cpu_si);
            long cores = (long) cpu_si.dwNumberOfProcessors;
#else
            long cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
            if (cores < 1) cores = 1;
            n = (int) cores;
            if (n > CAJETA_DEFAULT_CARRIERS_CAP) n = CAJETA_DEFAULT_CARRIERS_CAP;
        }
        if (n > CAJETA_MAX_CARRIERS) n = CAJETA_MAX_CARRIERS;
        __cajeta_carrier_count = n;
        for (int i = 0; i < n; ++i) {
            __cajeta_carriers[i].carrier_id = i;
            pthread_mutex_init(&__cajeta_carriers[i].deque_mutex, NULL);
            __cajeta_deque_init(&__cajeta_carriers[i].deque);
        }
        for (int i = 0; i < n; ++i) {
            pthread_create(&__cajeta_carriers[i].thread, NULL,
                           __cajeta_carrier_loop, &__cajeta_carriers[i]);
        }
    }
    pthread_mutex_unlock(&__cajeta_task_mutex);
    // Publish the new fiber. publish_ready acquires the target carrier's
    // deque_mutex itself; doing it outside __cajeta_task_mutex keeps the
    // pool → deque lock order (and avoids the recursive task_mutex grab
    // publish_ready does for the sleeper-signal step).
    __cajeta_publish_ready(f);
}

// Block until the task at `done_addr` flips to nonzero. From a fiber:
// park-yield-recheck; the carrier can run other fibers in between. From
// the main thread (no current fiber): condvar wait. This is what makes
// nested await work — a fiber awaiting another fiber doesn't hold the
// carrier hostage.
//
// R5-C: after each fiber wake, check the fiber's cancel_with marker.
// If set, the surrounding scope cancelled us — throw the trigger so
// the cancelled fiber's body unwinds and the trampoline catches it
// onto the Task's exception slot (where scope's next walk picks it up,
// but for cancellation siblings that's just a propagation of what the
// scope already decided to raise).
void __cajeta_task_wait(int32_t* done_addr) {
    if (!done_addr) return;
    if (__cajeta_current_fiber) {
        // C2: deliver a pending cancellation even when the awaited task is
        // ALREADY done: the loop below only re-checks cancel_with after a park,
        // which never happens if *done_addr is set on entry — so without this the
        // scope's cancel would be silently swallowed and the fiber run on.
        void* pending = __cajeta_current_fiber->cancel_with;
        if (pending) {
            __cajeta_current_fiber->cancel_with = NULL;
            __cajeta_throw(pending);
        }
        // Recheck *done_addr under the SAME mutex task_complete uses to set
        // it and drain parked fibers, then park atomically. If done flipped
        // before we acquired the lock, we see it here and never park — which
        // closes the lost-wakeup window (task_complete draining an empty
        // parked list, then this fiber parking with no future waker).
        for (;;) {
            pthread_mutex_lock(&__cajeta_task_mutex);
            if (*done_addr) {
                pthread_mutex_unlock(&__cajeta_task_mutex);
                break;
            }
            __cajeta_fiber_park_locked();  // releases the mutex, then swaps
            void* cancel = __cajeta_current_fiber->cancel_with;
            if (cancel) {
                __cajeta_current_fiber->cancel_with = NULL;
                __cajeta_throw(cancel);
            }
        }
        return;
    }
    pthread_mutex_lock(&__cajeta_task_mutex);
    while (!*done_addr) {
        pthread_cond_wait(&__cajeta_task_done_cond, &__cajeta_task_mutex);
    }
    pthread_mutex_unlock(&__cajeta_task_mutex);
}

// R5-C: set a fiber's cancel_with marker. Its next __cajeta_task_wait
// resume will throw the marker instead of returning normally. Idempotent —
// re-cancel just overwrites the marker (last cancel wins). NULL fiber
// is a no-op (caller may not have a fiber pointer if the task hasn't
// been dispatched yet — but cancel_with set on a not-yet-dispatched
// fiber is still honored as soon as it parks on its first await).
void __cajeta_fiber_cancel(struct cajeta_fiber* fiber, void* throwable) {
    if (!fiber) return;
    fiber->cancel_with = throwable;
}

// Called by the codegen-emitted trampoline once the inner fn has run and
// its result has been written into the task's value slot. Sets done = 1
// under the mutex, wakes any main-thread awaiter via cond_done, and moves
// every parked fiber back to the ready queue so they can recheck their
// own await condition.
void __cajeta_task_complete(int32_t* done_addr) {
    if (!done_addr) return;
    // Phase 1 — flip the done flag, wake any main-thread awaiters, and
    // detach the parked list (under pool_mutex only).
    pthread_mutex_lock(&__cajeta_task_mutex);
    *done_addr = 1;
    pthread_cond_broadcast(&__cajeta_task_done_cond);
    struct cajeta_fiber* woken = __cajeta_drain_parked_locked();
    pthread_mutex_unlock(&__cajeta_task_mutex);
    // Phase 2 — publish each woken parker WITHOUT holding pool_mutex.
    // publish_ready's lock order (deque → pool) would invert against
    // pool → deque if we did this inside Phase 1.
    while (woken) {
        struct cajeta_fiber* next = woken->next;
        __cajeta_publish_ready(woken);
        woken = next;
    }
}

// --- R9.1 — timer wheel + cooperative timeout -----------------------------
//
// Goal: let a fiber's __cajeta_task_wait honor a deadline. The intrinsic
// __cajeta_task_wait_timeout returns 1 if *done_addr flips before the
// deadline, 0 if the deadline expires first.
//
// Mechanism: a sorted singly-linked list of (deadline_ns, fiber) entries
// protected by __cajeta_task_mutex; a single timer thread (lazy-started on
// the first registration) sleeps via pthread_cond_timedwait until the next
// deadline; on expire it walks the list, detaches expired fibers from
// __cajeta_parked_head and publishes them. The fiber-side loop rechecks
// done_addr + deadline on every wake — wake-all spurious wakes from
// __cajeta_task_complete converge naturally (recheck rejects them) without
// needing a wake_reason CAS. The race to manage is "timer wake vs.
// concurrent wake-all": both take __cajeta_task_mutex and use parked-list
// membership as the gate, so the fiber is detached and published exactly
// once. If timer fires while the fiber isn't on parked_head (running, or
// already published), the timer entry is consumed (removed from the timer
// list) and the fiber catches the expired deadline via the now_ns check
// on its next park-loop iteration.
//
// Timer entries are owned by the FIBER'S STACK (one per __cajeta_task_wait_timeout
// call), so the timer thread never frees memory it didn't allocate. The fiber
// cancels its own entry before returning; cancel is idempotent against
// already-consumed entries via a linear walk of the live list.

#include <time.h>
#include <errno.h>

// Statics (head, cond, thread, started, shutdown_requested) declared
// above the carrier section so __cajeta_task_shutdown can join the
// timer thread on teardown; the struct definition follows here.
struct cajeta_timer_entry {
    int64_t deadline_ns;
    struct cajeta_fiber* fiber;
    struct cajeta_timer_entry* next;
};

static int64_t __cajeta_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000000LL + (int64_t) ts.tv_nsec;
}

// Insert entry into the sorted-by-deadline list. Caller holds __cajeta_task_mutex.
static void __cajeta_timer_insert_locked(struct cajeta_timer_entry* e) {
    struct cajeta_timer_entry** p = &__cajeta_timer_head;
    while (*p && (*p)->deadline_ns <= e->deadline_ns) p = &(*p)->next;
    e->next = *p;
    *p = e;
}

// Remove `f` from __cajeta_parked_head if present. Returns 1 if removed.
// Caller holds __cajeta_task_mutex.
static int __cajeta_parked_remove_locked(struct cajeta_fiber* f) {
    if (!__cajeta_parked_head) return 0;
    if (__cajeta_parked_head == f) {
        __cajeta_parked_head = f->next;
        f->next = NULL;
        return 1;
    }
    for (struct cajeta_fiber* p = __cajeta_parked_head; p->next; p = p->next) {
        if (p->next == f) {
            p->next = f->next;
            f->next = NULL;
            return 1;
        }
    }
    return 0;
}

// Timer thread. Sleeps on __cajeta_timer_cond until the next deadline (or
// a register/cancel/shutdown signal), wakes expired fibers, sleeps again.
static void* __cajeta_timer_loop(void* arg) {
    (void) arg;
    pthread_mutex_lock(&__cajeta_task_mutex);
    for (;;) {
        if (__cajeta_timer_shutdown_requested) {
            pthread_mutex_unlock(&__cajeta_task_mutex);
            return NULL;
        }
        int64_t now = __cajeta_now_ns();
        struct cajeta_fiber* to_publish = NULL;
        while (__cajeta_timer_head && __cajeta_timer_head->deadline_ns <= now) {
            struct cajeta_timer_entry* e = __cajeta_timer_head;
            __cajeta_timer_head = e->next;
            e->next = NULL;
            // Best-effort detach + publish. If fiber isn't on parked_head,
            // it's running (or already on a deque); the fiber's next park-
            // loop iteration will see the expired deadline.
            if (__cajeta_parked_remove_locked(e->fiber)) {
                e->fiber->next = to_publish;
                to_publish = e->fiber;
            }
        }
        if (to_publish) {
            pthread_mutex_unlock(&__cajeta_task_mutex);
            while (to_publish) {
                struct cajeta_fiber* next = to_publish->next;
                __cajeta_publish_ready(to_publish);
                to_publish = next;
            }
            pthread_mutex_lock(&__cajeta_task_mutex);
            continue;
        }
        if (__cajeta_timer_head) {
            int64_t deadline = __cajeta_timer_head->deadline_ns;
            struct timespec ts;
            ts.tv_sec = (time_t) (deadline / 1000000000LL);
            ts.tv_nsec = (long) (deadline % 1000000000LL);
            // CLOCK_REALTIME-based timedwait — the default. Deadline is
            // monotonic ns, which works regardless of clock choice for
            // an upper-bound sleep (the worst case is a stale clock
            // sleeping longer than needed; the fiber-side deadline check
            // catches up either way).
            pthread_cond_timedwait(&__cajeta_timer_cond,
                                    &__cajeta_task_mutex, &ts);
        } else {
            pthread_cond_wait(&__cajeta_timer_cond, &__cajeta_task_mutex);
        }
    }
}

// Lazy-start the timer thread on first registration. Caller holds task_mutex.
static void __cajeta_timer_ensure_started_locked(void) {
    if (__cajeta_timer_started) return;
    __cajeta_timer_started = 1;
    pthread_create(&__cajeta_timer_thread, NULL, __cajeta_timer_loop, NULL);
}

// Cancel a timer entry. No-op if already consumed by the timer thread
// (not in the live list anymore). Caller must NOT hold task_mutex.
static void __cajeta_timer_cancel(struct cajeta_timer_entry* entry) {
    pthread_mutex_lock(&__cajeta_task_mutex);
    struct cajeta_timer_entry** p = &__cajeta_timer_head;
    while (*p) {
        if (*p == entry) {
            *p = entry->next;
            entry->next = NULL;
            break;
        }
        p = &(*p)->next;
    }
    pthread_mutex_unlock(&__cajeta_task_mutex);
}

// R9.1 intrinsic — block until *done_addr flips OR deadline_ns is reached.
// Returns 1 on done, 0 on timeout. Mirrors __cajeta_task_wait but adds a
// per-call timer entry that wakes the fiber at the deadline. deadline_ns
// is a CLOCK_MONOTONIC absolute timestamp (use __cajeta_currentTimeNanos
// to compute one from a duration).
int32_t __cajeta_task_wait_timeout(int32_t* done_addr, int64_t deadline_ns) {
    if (!done_addr) return 1;
    if (!__cajeta_current_fiber) {
        // Main thread / non-fiber caller: cond_timedwait on the existing
        // task-done condvar. The condvar's clock is CLOCK_REALTIME; convert
        // our monotonic deadline by computing the remaining nanos and
        // adding to a fresh REALTIME `now`. Sufficient for the timeout
        // ceiling; precision-critical use should switch to a CLOCK_MONOTONIC
        // condvar (pthread_condattr_setclock) when a user surfaces a need.
        pthread_mutex_lock(&__cajeta_task_mutex);
        while (!*done_addr) {
            int64_t mono_now = __cajeta_now_ns();
            if (mono_now >= deadline_ns) {
                pthread_mutex_unlock(&__cajeta_task_mutex);
                return 0;
            }
            int64_t remaining_ns = deadline_ns - mono_now;
            struct timespec real_ts;
            clock_gettime(CLOCK_REALTIME, &real_ts);
            int64_t real_deadline = (int64_t) real_ts.tv_sec * 1000000000LL
                                  + (int64_t) real_ts.tv_nsec
                                  + remaining_ns;
            real_ts.tv_sec = (time_t) (real_deadline / 1000000000LL);
            real_ts.tv_nsec = (long) (real_deadline % 1000000000LL);
            int rc = pthread_cond_timedwait(&__cajeta_task_done_cond,
                                             &__cajeta_task_mutex,
                                             &real_ts);
            if (rc == ETIMEDOUT) {
                pthread_mutex_unlock(&__cajeta_task_mutex);
                return 0;
            }
        }
        pthread_mutex_unlock(&__cajeta_task_mutex);
        return 1;
    }
    // Fiber path. Stack-local entry: lifetime = this function, which is
    // safe because the fiber's stack persists across park/resume.
    struct cajeta_timer_entry entry;
    entry.deadline_ns = deadline_ns;
    entry.fiber = __cajeta_current_fiber;
    entry.next = NULL;
    pthread_mutex_lock(&__cajeta_task_mutex);
    __cajeta_timer_ensure_started_locked();
    int signal_timer = (__cajeta_timer_head == NULL
                        || __cajeta_timer_head->deadline_ns > deadline_ns);
    __cajeta_timer_insert_locked(&entry);
    pthread_mutex_unlock(&__cajeta_task_mutex);
    if (signal_timer) {
        pthread_cond_signal(&__cajeta_timer_cond);
    }
    for (;;) {
        // Recheck both wake conditions (task done / deadline passed) under
        // the mutex that task_complete and the timer thread use, then park
        // atomically — same lost-wakeup fix as __cajeta_task_wait. The timer
        // thread, if it fires before we park, can't find us on parked_head
        // and consumes our entry; we observe the elapsed deadline here and
        // return 0 instead of parking with no waker left.
        pthread_mutex_lock(&__cajeta_task_mutex);
        if (*done_addr) {
            pthread_mutex_unlock(&__cajeta_task_mutex);
            __cajeta_timer_cancel(&entry);
            return 1;
        }
        if (__cajeta_now_ns() >= deadline_ns) {
            pthread_mutex_unlock(&__cajeta_task_mutex);
            __cajeta_timer_cancel(&entry);
            return 0;
        }
        __cajeta_fiber_park_locked();  // releases the mutex, then swaps
        // Mirror __cajeta_task_wait's R5-C cancel handling: a scope's
        // first-throw escalation may have set cancel_with while we were
        // parked; honor it before looping (we still cancel our timer so
        // the entry doesn't outlive this fiber's stack frame).
        void* cancel = __cajeta_current_fiber->cancel_with;
        if (cancel) {
            __cajeta_current_fiber->cancel_with = NULL;
            __cajeta_timer_cancel(&entry);
            __cajeta_throw(cancel);
        }
    }
}

// R9.1 intrinsic — current CLOCK_MONOTONIC nanoseconds. Surfaced for stdlib
// computing a deadline from a Duration (R9.3): `now() + d.toNanos()`.
int64_t __cajeta_currentTimeNanos(void) {
    return __cajeta_now_ns();
}

// R9.5 — fiber-aware sleep. Park the running fiber on the timer wheel for
// up to `nanos` nanoseconds. Built on top of __cajeta_task_wait_timeout
// by feeding it a sentinel done_addr that never flips — the wait then
// always resolves via the deadline. From the main thread / non-fiber
// caller, cond_timedwait on the same condvar (same fallback the timeout
// path takes). Used by Channel.select's polling backoff; available to
// other callers wanting a cooperative sleep.
void __cajeta_fiber_sleep_nanos(int64_t nanos) {
    if (nanos <= 0) return;
    int32_t never = 0;
    int64_t deadline = __cajeta_now_ns() + nanos;
    (void) __cajeta_task_wait_timeout(&never, deadline);
}

// --- R9.4 — I/O reactor / netpoller -----------------------------------------
//
// Goal: park a fiber on file-descriptor readiness without freezing the
// carrier OS thread. The intrinsic __cajeta_io_wait(fd, events_mask) blocks
// the calling fiber until any of the requested events fires on `fd`, then
// returns 1; non-fiber callers fall through to a direct blocking
// epoll_wait so the surface API is uniform across both contexts.
//
// Mechanism: a single epoll fd owned by a dedicated reactor thread.
// __cajeta_io_wait registers (fd, requested events, current fiber) with
// the reactor and parks. The reactor's epoll_wait loop wakes, finds the
// matching waiter(s), detaches each fiber from __cajeta_parked_head, and
// republishes via __cajeta_publish_ready. EPOLLONESHOT ensures each fd
// auto-cleans from epoll after firing; the per-call waiter struct lives
// on the heap (single per fiber, multiple fibers may wait on different
// fds) and is freed by the wake path.
//
// v1 limitations:
//   - Linux only. macOS (kqueue) and Windows (IOCP) are stubbed; the
//     intrinsic returns -1 there.
//   - Each io_wait call sets up its own epoll registration; long-lived
//     persistent registrations (the standard netpoller pattern for high
//     fd counts) come when a real consumer surfaces.
//   - No deadline parameter v1. Deadlines compose with the R9.1 timer —
//     a withIoTimeout(d, fd, events) helper at the cajeta level can layer
//     both. Deferred until a use case lands.
//   - One waiter per fd in v1. Two fibers waiting on the same fd would
//     have only one notified (whichever the epoll_ctl_add saw second
//     would EEXIST and we degrade to MOD). Real netpoller semantics
//     (separate read/write waiter queues per fd) lands later.

#if defined(__linux__)
#  include <sys/epoll.h>
#  include <sys/eventfd.h>
#endif

#define CAJETA_IO_READ  1
#define CAJETA_IO_WRITE 2

struct cajeta_io_waiter {
    int fd;
    int events;
    struct cajeta_fiber* fiber;
    struct cajeta_io_waiter* next;
};

#if defined(__linux__)

static int __cajeta_io_events_to_epoll(int events) {
    int e = 0;
    if (events & CAJETA_IO_READ)  e |= EPOLLIN;
    if (events & CAJETA_IO_WRITE) e |= EPOLLOUT;
    return e | EPOLLONESHOT;
}

// Reactor thread main loop. Polls epoll_wait with a 1-second timeout so
// the shutdown flag is observed even when no I/O is in flight. On each
// ready event, walks the waiter list under task_mutex, detaches matched
// fibers from __cajeta_parked_head, and publishes them.
static void* __cajeta_reactor_loop(void* arg) {
    (void) arg;
    for (;;) {
        if (__cajeta_reactor_shutdown_requested) return NULL;
        struct epoll_event ep[64];
        int n = epoll_wait(__cajeta_reactor_epfd, ep, 64, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            return NULL;
        }
        if (n == 0) continue;
        struct cajeta_fiber* to_publish = NULL;
        pthread_mutex_lock(&__cajeta_task_mutex);
        for (int i = 0; i < n; ++i) {
            int fd = ep[i].data.fd;
            struct cajeta_io_waiter** p = &__cajeta_reactor_waiters;
            while (*p) {
                if ((*p)->fd == fd) {
                    struct cajeta_io_waiter* w = *p;
                    *p = w->next;
                    // The fd auto-removed itself from epoll via
                    // EPOLLONESHOT; just need to detach + publish the
                    // fiber.
                    if (__cajeta_parked_remove_locked(w->fiber)) {
                        w->fiber->next = to_publish;
                        to_publish = w->fiber;
                    }
                    free(w);
                } else {
                    p = &(*p)->next;
                }
            }
        }
        pthread_mutex_unlock(&__cajeta_task_mutex);
        while (to_publish) {
            struct cajeta_fiber* next = to_publish->next;
            __cajeta_publish_ready(to_publish);
            to_publish = next;
        }
    }
}

// Lazy-start the reactor on first registration. Caller holds task_mutex.
static void __cajeta_reactor_ensure_started_locked(void) {
    if (__cajeta_reactor_started) return;
    __cajeta_reactor_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (__cajeta_reactor_epfd < 0) {
        fprintf(stderr, "cajeta: epoll_create1 failed: %d\n", errno);
        return;
    }
    __cajeta_reactor_started = 1;
    pthread_create(&__cajeta_reactor_thread, NULL,
                    __cajeta_reactor_loop, NULL);
}

// Cancel a waiter (clear the registration) on the error/cleanup path.
// Caller holds task_mutex; entry must NOT have been published yet.
static void __cajeta_reactor_cancel_locked(struct cajeta_io_waiter* w) {
    struct cajeta_io_waiter** p = &__cajeta_reactor_waiters;
    while (*p) {
        if (*p == w) { *p = w->next; break; }
        p = &(*p)->next;
    }
    epoll_ctl(__cajeta_reactor_epfd, EPOLL_CTL_DEL, w->fd, NULL);
}

int32_t __cajeta_io_wait(int32_t fd, int32_t events) {
    if (!__cajeta_current_fiber) {
        // Non-fiber caller: skip the reactor and just do a direct
        // blocking epoll_wait. Same observable surface — 1 on ready,
        // 0/-1 on error — without paying for reactor lazy-start.
        int epfd = epoll_create1(EPOLL_CLOEXEC);
        if (epfd < 0) return -1;
        struct epoll_event ep;
        ep.events = __cajeta_io_events_to_epoll(events);
        ep.data.fd = fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ep) < 0) {
            close(epfd);
            return -1;
        }
        struct epoll_event got;
        int n;
        do {
            n = epoll_wait(epfd, &got, 1, -1);
        } while (n < 0 && errno == EINTR);
        close(epfd);
        return (n > 0) ? 1 : 0;
    }
    pthread_mutex_lock(&__cajeta_task_mutex);
    __cajeta_reactor_ensure_started_locked();
    if (!__cajeta_reactor_started) {
        pthread_mutex_unlock(&__cajeta_task_mutex);
        return -1;
    }
    struct cajeta_io_waiter* w = malloc(sizeof(*w));
    if (!w) {
        pthread_mutex_unlock(&__cajeta_task_mutex);
        return -1;
    }
    w->fd = fd;
    w->events = events;
    w->fiber = __cajeta_current_fiber;
    w->next = __cajeta_reactor_waiters;
    __cajeta_reactor_waiters = w;
    struct epoll_event ep;
    ep.events = __cajeta_io_events_to_epoll(events);
    ep.data.fd = fd;
    int rc = epoll_ctl(__cajeta_reactor_epfd, EPOLL_CTL_ADD, fd, &ep);
    if (rc < 0 && errno == EEXIST) {
        rc = epoll_ctl(__cajeta_reactor_epfd, EPOLL_CTL_MOD, fd, &ep);
    }
    if (rc < 0) {
        __cajeta_reactor_cancel_locked(w);
        free(w);
        pthread_mutex_unlock(&__cajeta_task_mutex);
        return -1;
    }
    // Park while STILL holding task_mutex (park_locked releases it). The
    // waiter was registered under this same lock, so by the time the mutex
    // is dropped the fiber is already on parked_head — the reactor thread,
    // which needs task_mutex to match + wake waiters, therefore cannot fire
    // and free our waiter in the gap before we park (the EPOLLONESHOT event
    // is one-shot, so a missed wake would hang the fiber permanently).
    __cajeta_fiber_park_locked();
    return 1;
}

// Linux eventfd surface, exposed for test bring-up and for cooperative
// cross-fiber signalling. eventfd is a counter the kernel guarantees is
// edge-sensitive on writes — perfect for the "one-shot ready" pattern
// the I/O reactor needs to verify end-to-end.
int32_t __cajeta_eventfd_create(void) {
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    return (fd < 0) ? -1 : (int32_t) fd;
}

int32_t __cajeta_eventfd_signal(int32_t fd) {
    uint64_t one = 1;
    ssize_t n = write(fd, &one, sizeof(one));
    return (n == (ssize_t) sizeof(one)) ? 0 : -1;
}

int64_t __cajeta_eventfd_consume(int32_t fd) {
    uint64_t buf = 0;
    ssize_t n = read(fd, &buf, sizeof(buf));
    return (n == (ssize_t) sizeof(buf)) ? (int64_t) buf : -1;
}

int32_t __cajeta_fd_close(int32_t fd) {
    return close(fd);
}

#else /* !__linux__ */

int32_t __cajeta_io_wait(int32_t fd, int32_t events) {
    (void) fd; (void) events;
    fprintf(stderr, "cajeta: __cajeta_io_wait not yet implemented on this platform\n");
    return -1;
}
int32_t __cajeta_eventfd_create(void) {
    fprintf(stderr, "cajeta: __cajeta_eventfd_create requires Linux\n");
    return -1;
}
int32_t __cajeta_eventfd_signal(int32_t fd) { (void) fd; return -1; }
int64_t __cajeta_eventfd_consume(int32_t fd) { (void) fd; return -1; }
int32_t __cajeta_fd_close(int32_t fd) { (void) fd; return -1; }

#endif /* __linux__ */

// --- Threading: scope frames (R5-A) ---------------------------------------
//
// `scope { ... }` blocks own their child tasks: codegen emits a
// __cajeta_scope_enter at the opening brace, every spawn site inside
// calls __cajeta_scope_register with its task's done-addr, and the
// closing brace runs __cajeta_scope_exit which waits for every
// registered task's done flag before letting control past `}`.
//
// The scope chain is per-fiber (fibers running on the same carrier OS
// thread must not see each other's scopes); the main thread has its
// own TLS chain. The __cajeta_scope_top_ptr helper picks the right
// slot transparently.

void __cajeta_scope_enter(void) {
    struct cajeta_scope_frame* f =
        (struct cajeta_scope_frame*) malloc(sizeof(*f));
    if (!f) {
        fprintf(stderr, "cajeta: __cajeta_scope_enter malloc failed\n");
        abort();
    }
    f->entries = NULL;
    f->count = 0;
    f->cap = 0;
    struct cajeta_scope_frame** top = __cajeta_scope_top_ptr();
    f->prev = *top;
    *top = f;
}

// Append a task's (done, exception) pair to the current scope frame.
// spawn sites call this just before __cajeta_task_run; the corresponding
// scope_exit waits on done then inspects exception. Doc behavior: spawn
// outside any scope is a compile error — but today's MVP doesn't enforce
// that, so register-without-scope is a no-op rather than an abort
// (preserving the existing tests' top-level-spawn pattern).
void __cajeta_scope_register(int32_t* done_addr, void** exception_addr,
                             void** fiber_slot) {
    struct cajeta_scope_frame** top = __cajeta_scope_top_ptr();
    struct cajeta_scope_frame* f = *top;
    if (!f) return;
    if (f->count == f->cap) {
        int newcap = f->cap ? f->cap * 2 : 4;
        struct cajeta_scope_entry* grown =
            (struct cajeta_scope_entry*) realloc(f->entries,
                newcap * sizeof(struct cajeta_scope_entry));
        if (!grown) {
            fprintf(stderr, "cajeta: __cajeta_scope_register realloc failed\n");
            abort();
        }
        f->entries = grown;
        f->cap = newcap;
    }
    f->entries[f->count].done_addr = done_addr;
    f->entries[f->count].exception_addr = exception_addr;
    f->entries[f->count].fiber_slot = fiber_slot;
    f->count++;
}

// Remove any scope_register entries whose done_addr falls inside the given
// task struct. Called by the task drop function so a re-throw via await
// (which unwinds the drop chain and frees the task before scope_exit_to
// runs) doesn't leave scope_exit_to dereferencing freed memory. The
// freed-task range used to be exception_addr (one slot inside the task) —
// matching any field-of-task pointer is the same call.
void __cajeta_scope_deregister_task(void* task_ptr, uint64_t task_size) {
    if (!task_ptr) return;
    char* lo = (char*) task_ptr;
    char* hi = lo + task_size;
    for (struct cajeta_scope_frame* f = *__cajeta_scope_top_ptr();
            f; f = f->prev) {
        for (int i = 0; i < f->count; i++) {
            char* d = (char*) f->entries[i].done_addr;
            if (d >= lo && d < hi) {
                f->entries[i].done_addr = NULL;
                f->entries[i].exception_addr = NULL;
                f->entries[i].fiber_slot = NULL;
            }
        }
    }
}

// Wait for every registered task's done flag, then walk again checking
// exception slots. If any task threw, re-raise the first one found —
// the doc's "first-throw wins" semantics for scope joins.
//
// R5-D-lite: no sibling cancellation yet. Pre-cancel R5-C lands, this
// just waits for every child to complete naturally, then escalates.
// When R5-C lands, the loop becomes: on first non-null exception,
// cancel the rest, wait for them, then re-raise.
static void __cajeta_scope_release(struct cajeta_scope_frame* f) {
    free(f->entries);
    free(f);
}

void __cajeta_scope_exit(void) {
    struct cajeta_scope_frame** top = __cajeta_scope_top_ptr();
    struct cajeta_scope_frame* f = *top;
    if (!f) return;
    // First-throw-wins escalation with R5-C cancellation: wait on each
    // child in turn; the moment one's exception slot is non-null, mark
    // that as the trigger and cancel every remaining (unawaited) child
    // so their next await aborts. Then keep waiting on the rest so the
    // scope still joins everything before unwinding upward.
    void* trigger = NULL;
    for (int i = 0; i < f->count; i++) {
        __cajeta_task_wait(f->entries[i].done_addr);
        if (!trigger && f->entries[i].exception_addr
                && *f->entries[i].exception_addr) {
            trigger = *f->entries[i].exception_addr;
            // C2: read each sibling's fiber slot + cancel under the task mutex so
            // it can't race the carrier nulling/freeing a completed fiber.
            pthread_mutex_lock(&__cajeta_task_mutex);
            for (int j = i + 1; j < f->count; j++) {
                if (f->entries[j].fiber_slot && *f->entries[j].fiber_slot) {
                    __cajeta_fiber_cancel(
                        (struct cajeta_fiber*) *f->entries[j].fiber_slot,
                        trigger);
                }
            }
            pthread_mutex_unlock(&__cajeta_task_mutex);
        }
    }
    *top = f->prev;
    __cajeta_scope_release(f);
    if (trigger) {
        __cajeta_throw(trigger);
    }
}

// Watermark API for the R5-A' implicit function-body scope. Codegen
// captures the scope_top observed at function entry, then calls
// __cajeta_scope_exit_to with that watermark on every return path.
// Pops every frame above the watermark in LIFO order — handles the
// case where `return` exits from inside one or more explicit
// `scope { }` blocks nested under the function body's implicit scope.
void* __cajeta_scope_save_top(void) {
    struct cajeta_scope_frame** top = __cajeta_scope_top_ptr();
    return (void*) *top;
}

void __cajeta_scope_exit_to(void* watermark) {
    struct cajeta_scope_frame** top = __cajeta_scope_top_ptr();
    // Collect the first trigger encountered while popping frames so we
    // can re-raise it at the end. Walking innermost-out matches "child
    // exceptions surface up to the enclosing scope"; the topmost frame's
    // throws win over outer frames because we process them first. R5-C:
    // cancel remaining siblings within each frame as soon as we see a
    // trigger from it, same as scope_exit.
    void* trigger = NULL;
    while (*top != (struct cajeta_scope_frame*) watermark) {
        struct cajeta_scope_frame* f = *top;
        if (!f) break;
        void* frame_trigger = NULL;
        for (int i = 0; i < f->count; i++) {
            __cajeta_task_wait(f->entries[i].done_addr);
            if (!frame_trigger && f->entries[i].exception_addr
                    && *f->entries[i].exception_addr) {
                frame_trigger = *f->entries[i].exception_addr;
                // C2: same mutex discipline as scope_exit's cancel loop.
                pthread_mutex_lock(&__cajeta_task_mutex);
                for (int j = i + 1; j < f->count; j++) {
                    if (f->entries[j].fiber_slot && *f->entries[j].fiber_slot) {
                        __cajeta_fiber_cancel(
                            (struct cajeta_fiber*) *f->entries[j].fiber_slot,
                            frame_trigger);
                    }
                }
                pthread_mutex_unlock(&__cajeta_task_mutex);
            }
        }
        if (!trigger && frame_trigger) trigger = frame_trigger;
        *top = f->prev;
        __cajeta_scope_release(f);
    }
    if (trigger) {
        __cajeta_throw(trigger);
    }
}

// --- Threading sync primitives: Lock (async-aware, R4) -------------------
//
// Sits on top of the fiber executor. A Cajeta Lock is a pthread_mutex_t
// for state protection plus a per-lock wait queue of parked fibers and a
// condvar for any main-thread waiter. Acquire from a fiber that hits a
// held lock parks the fiber on the lock's queue and yields — the carrier
// keeps running other fibers. Acquire from the main thread cond_waits
// like a regular pthread (main is outside the fiber executor and can OS-
// block without harming concurrency).
//
// `held` is the canonical "is anyone holding this lock" flag, distinct
// from the pthread_mutex_t state (which protects the lock's own metadata
// — held + wait queue — not user mutual exclusion). Splitting them lets
// the fiber path park on the wait queue without leaving the user-mutex
// held.

struct cajeta_async_lock {
    pthread_mutex_t mutex;
    pthread_cond_t released_cond;
    int held;
    struct cajeta_fiber* wait_head;
    struct cajeta_fiber* wait_tail;
};

void* __cajeta_lock_new(void) {
    struct cajeta_async_lock* l = (struct cajeta_async_lock*) malloc(sizeof(*l));
    if (!l) {
        fprintf(stderr, "cajeta: __cajeta_lock_new failed\n");
        abort();
    }
    if (pthread_mutex_init(&l->mutex, NULL) != 0) {
        fprintf(stderr, "cajeta: pthread_mutex_init failed\n");
        free(l);
        abort();
    }
    if (pthread_cond_init(&l->released_cond, NULL) != 0) {
        fprintf(stderr, "cajeta: pthread_cond_init failed\n");
        pthread_mutex_destroy(&l->mutex);
        free(l);
        abort();
    }
    l->held = 0;
    l->wait_head = NULL;
    l->wait_tail = NULL;
    return l;
}

void __cajeta_lock_acquire(void* p) {
    if (!p) return;
    struct cajeta_async_lock* l = (struct cajeta_async_lock*) p;
    if (!__cajeta_current_fiber) {
        // Main thread (or any non-fiber pthread): cond_wait until the
        // holder releases. Matches the doc's intent that the program's
        // entry point can sit on a top-level await without burning CPU.
        pthread_mutex_lock(&l->mutex);
        while (l->held) {
            pthread_cond_wait(&l->released_cond, &l->mutex);
        }
        l->held = 1;
        pthread_mutex_unlock(&l->mutex);
        return;
    }
    // Fiber path: park on the lock's wait queue if held. On wake the
    // dequeued-by-release fiber is back on the ready queue; when the
    // carrier dispatches it, swapcontext returns INTO this loop and
    // re-checks held (another acquirer could have raced in, so re-park
    // rather than assume we have the lock).
    for (;;) {
        pthread_mutex_lock(&l->mutex);
        struct cajeta_fiber* self = __cajeta_current_fiber;
        // H7: honor a cancellation delivered while parked on this lock — throw
        // instead of acquiring and running the critical section uncancelled. If we
        // were just handed the lock (held==0, i.e. a release woke us rather than
        // another acquirer racing in), pass the handoff to the next waiter first
        // so it isn't stranded.
        if (self->cancel_with) {
            void* cw = self->cancel_with;
            self->cancel_with = NULL;
            struct cajeta_fiber* nxt = NULL;
            if (!l->held) {
                nxt = l->wait_head;
                if (nxt) {
                    l->wait_head = nxt->next;
                    if (!l->wait_head) l->wait_tail = NULL;
                    nxt->next = NULL;
                }
            }
            pthread_mutex_unlock(&l->mutex);
            if (nxt) {
                // Wake the handed-off waiter through the work-stealing scheduler:
                // publish_ready routes the (already-started) fiber to its home
                // carrier's deque and signals a sleeper. It does its own locking,
                // so it must be called OUTSIDE __cajeta_task_mutex.
                __cajeta_publish_ready(nxt);
            }
            __cajeta_throw(cw);
        }
        if (!l->held) {
            l->held = 1;
            pthread_mutex_unlock(&l->mutex);
            return;
        }
        self->next = NULL;
        if (l->wait_tail) {
            l->wait_tail->next = self;
            l->wait_tail = self;
        } else {
            l->wait_head = self;
            l->wait_tail = self;
        }
        self->state = CAJETA_FIBER_PARKED;
        pthread_mutex_unlock(&l->mutex);
        __cajeta_swapcontext(&self->ctx, &__cajeta_carrier_ctx);
    }
}

void __cajeta_lock_release(void* p) {
    if (!p) return;
    struct cajeta_async_lock* l = (struct cajeta_async_lock*) p;
    pthread_mutex_lock(&l->mutex);
    l->held = 0;
    struct cajeta_fiber* next = l->wait_head;
    if (next) {
        l->wait_head = next->next;
        if (!l->wait_head) l->wait_tail = NULL;
        next->next = NULL;
    }
    // Signal one main-thread waiter even when a fiber is being handed
    // the lock — costs almost nothing and avoids missed-wake races with
    // a pthread that happens to be cond_waiting concurrently.
    pthread_cond_signal(&l->released_cond);
    pthread_mutex_unlock(&l->mutex);
    if (next) {
        // Publish the woken fiber onto a carrier's deque (current carrier
        // if running on one, else carrier 0). publish_ready takes the
        // per-carrier deque_mutex + signals a sleeping carrier through
        // the pool condvar.
        __cajeta_publish_ready(next);
    }
}

// Returns 1 if acquired, 0 if already held. Non-blocking even on a
// fiber — `tryAcquire` semantics are "give me the lock right now or
// tell me you couldn't", never "park me".
int32_t __cajeta_lock_try_acquire(void* p) {
    if (!p) return 0;
    struct cajeta_async_lock* l = (struct cajeta_async_lock*) p;
    int got = 0;
    pthread_mutex_lock(&l->mutex);
    if (!l->held) {
        l->held = 1;
        got = 1;
    }
    pthread_mutex_unlock(&l->mutex);
    return got;
}

void __cajeta_lock_destroy(void* p) {
    if (!p) return;
    struct cajeta_async_lock* l = (struct cajeta_async_lock*) p;
    // H8: destroying a lock that is still held or has parked waiters is UB
    // (pthread_*_destroy on a busy primitive), strands the waiting fibers, and
    // UAFs any in-flight/subsequent release that dereferences `l`. Refuse and leak
    // it rather than corrupt — a destroy-while-busy is a program bug.
    pthread_mutex_lock(&l->mutex);
    int busy = l->held || l->wait_head != NULL;
    pthread_mutex_unlock(&l->mutex);
    if (busy) {
        fprintf(stderr, "cajeta: Lock destroyed while held or with waiters; "
                "leaking it to avoid undefined behavior\n");
        return;
    }
    if (pthread_cond_destroy(&l->released_cond) != 0 ||
        pthread_mutex_destroy(&l->mutex) != 0) {
        fprintf(stderr, "cajeta: Lock primitive still busy at destroy; leaked\n");
        return;
    }
    free(l);
}

// --- Threading sync primitives: condition variable (R7-B) ----------------
//
// Fiber-aware condvar, paired with a Lock handle. `Mutex<T>.withLockWhen`
// (cajeta-docs/stdlib/Thread.md § Mutex) builds on it: wait-for-predicate
// inside a held lock, with notify on every mutation. A single condvar with
// notify-all + per-waiter predicate re-check is the v1 strategy (spurious
// wakeups are possible but harmless — each waiter re-tests its own
// condition).
//
// Same single-carrier discipline as the Lock: a fiber waiter enqueues on
// the condvar's own wait queue and swaps to the carrier; a main-thread
// waiter blocks on the condvar's pthread cond. notify_all moves every fiber
// waiter onto the carrier ready queue and broadcasts the pthread cond.

struct cajeta_async_condvar {
    pthread_mutex_t mutex;        // protects the wait queue; serializes with notify
    pthread_cond_t  main_cond;    // main-thread waiters block here
    struct cajeta_fiber* wait_head;
    struct cajeta_fiber* wait_tail;
};

void* __cajeta_condvar_new(void) {
    struct cajeta_async_condvar* cv =
        (struct cajeta_async_condvar*) malloc(sizeof(*cv));
    if (!cv) {
        fprintf(stderr, "cajeta: __cajeta_condvar_new failed\n");
        abort();
    }
    if (pthread_mutex_init(&cv->mutex, NULL) != 0) {
        fprintf(stderr, "cajeta: condvar pthread_mutex_init failed\n");
        free(cv);
        abort();
    }
    if (pthread_cond_init(&cv->main_cond, NULL) != 0) {
        fprintf(stderr, "cajeta: condvar pthread_cond_init failed\n");
        pthread_mutex_destroy(&cv->mutex);
        free(cv);
        abort();
    }
    cv->wait_head = NULL;
    cv->wait_tail = NULL;
    return cv;
}

// Atomically release `lockp`, suspend until notified, then reacquire `lockp`.
// The caller MUST hold `lockp` on entry; it holds it again on return.
void __cajeta_condvar_wait(void* cvp, void* lockp) {
    if (!cvp || !lockp) return;
    struct cajeta_async_condvar* cv = (struct cajeta_async_condvar*) cvp;
    if (__cajeta_current_fiber) {
        // Fiber path: enqueue self on the condvar wait queue, release the
        // user lock, then swap to the carrier. Enqueue happens under
        // cv->mutex BEFORE the lock release so a concurrent notify_all
        // (which also takes cv->mutex) can't slip between enqueue and
        // release and lose the wakeup — once we're on the queue, notify
        // will move us to the ready queue and we'll be resumed.
        struct cajeta_fiber* self = __cajeta_current_fiber;
        pthread_mutex_lock(&cv->mutex);
        self->next = NULL;
        if (cv->wait_tail) {
            cv->wait_tail->next = self;
            cv->wait_tail = self;
        } else {
            cv->wait_head = self;
            cv->wait_tail = self;
        }
        self->state = CAJETA_FIBER_PARKED;
        pthread_mutex_unlock(&cv->mutex);
        __cajeta_lock_release(lockp);
        __cajeta_swapcontext(&self->ctx, &__cajeta_carrier_ctx);
        // Resumed by notify_all (carrier re-dispatched us). Reacquire the
        // user lock before returning — may itself park if the lock is held.
        __cajeta_lock_acquire(lockp);
        return;
    }
    // Main-thread path: classic cond_wait. Release the user lock, block on
    // the condvar's own pthread cond, reacquire on wake. cv->mutex guards
    // the wait so it pairs with notify_all's broadcast under the same mutex.
    pthread_mutex_lock(&cv->mutex);
    __cajeta_lock_release(lockp);
    pthread_cond_wait(&cv->main_cond, &cv->mutex);
    pthread_mutex_unlock(&cv->mutex);
    __cajeta_lock_acquire(lockp);
}

// Wake every waiter: move all fiber waiters onto the carrier ready queue and
// broadcast the main-thread cond. Woken waiters re-check their predicate.
void __cajeta_condvar_notify_all(void* cvp) {
    if (!cvp) return;
    struct cajeta_async_condvar* cv = (struct cajeta_async_condvar*) cvp;
    pthread_mutex_lock(&cv->mutex);
    struct cajeta_fiber* w = cv->wait_head;
    cv->wait_head = NULL;
    cv->wait_tail = NULL;
    pthread_cond_broadcast(&cv->main_cond);
    pthread_mutex_unlock(&cv->mutex);
    while (w) {
        struct cajeta_fiber* next = w->next;
        __cajeta_publish_ready(w);
        w = next;
    }
}

void __cajeta_condvar_destroy(void* cvp) {
    if (!cvp) return;
    struct cajeta_async_condvar* cv = (struct cajeta_async_condvar*) cvp;
    pthread_cond_destroy(&cv->main_cond);
    pthread_mutex_destroy(&cv->mutex);
    free(cv);
}

// --- Threading sync primitives: reader-writer lock (R7-D) ----------------
//
// Fiber-aware RW lock backing `RwLock<T>` (cajeta-docs/stdlib/Thread.md §
// RwLock). Many readers may hold it concurrently; a writer holds it
// exclusively. Writer-preference: a reader blocks while any writer is
// waiting, so a steady stream of readers can't starve a writer.
//
// Same single-carrier discipline as Lock/condvar: a blocked fiber parks on
// the appropriate wait queue and swaps to the carrier; a blocked main
// thread cond_waits. Unlock wakes ALL waiters (writers first) and lets each
// re-check its predicate — spurious wakeups are harmless, matching the
// condvar's notify-all strategy. Under the single carrier this costs little.

// Publish a NULL-terminated fiber list onto the carrier pool's deques.
// Each fiber is routed via __cajeta_publish_ready, which picks the
// current carrier (if any) or carrier 0 and takes that carrier's
// deque_mutex. Shared by the rwlock unlock paths.
static void __cajeta_ready_enqueue_list(struct cajeta_fiber* head) {
    while (head) {
        struct cajeta_fiber* next = head->next;
        __cajeta_publish_ready(head);
        head = next;
    }
}

struct cajeta_async_rwlock {
    pthread_mutex_t mutex;        // protects state + wait queues
    pthread_cond_t  main_cond;    // main-thread readers + writers block here
    int readers;                  // active shared-read holders
    int writer;                   // 1 if a writer holds it exclusively
    int writers_waiting;          // queued writers (drives writer-preference)
    struct cajeta_fiber* rwait_head;   // fiber readers waiting
    struct cajeta_fiber* rwait_tail;
    struct cajeta_fiber* wwait_head;   // fiber writers waiting
    struct cajeta_fiber* wwait_tail;
};

void* __cajeta_rwlock_new(void) {
    struct cajeta_async_rwlock* rw =
        (struct cajeta_async_rwlock*) malloc(sizeof(*rw));
    if (!rw) {
        fprintf(stderr, "cajeta: __cajeta_rwlock_new failed\n");
        abort();
    }
    if (pthread_mutex_init(&rw->mutex, NULL) != 0) {
        fprintf(stderr, "cajeta: rwlock pthread_mutex_init failed\n");
        free(rw);
        abort();
    }
    if (pthread_cond_init(&rw->main_cond, NULL) != 0) {
        fprintf(stderr, "cajeta: rwlock pthread_cond_init failed\n");
        pthread_mutex_destroy(&rw->mutex);
        free(rw);
        abort();
    }
    rw->readers = 0;
    rw->writer = 0;
    rw->writers_waiting = 0;
    rw->rwait_head = NULL;
    rw->rwait_tail = NULL;
    rw->wwait_head = NULL;
    rw->wwait_tail = NULL;
    return rw;
}

void __cajeta_rwlock_rdlock(void* p) {
    if (!p) return;
    struct cajeta_async_rwlock* rw = (struct cajeta_async_rwlock*) p;
    if (!__cajeta_current_fiber) {
        pthread_mutex_lock(&rw->mutex);
        while (rw->writer || rw->writers_waiting > 0) {
            pthread_cond_wait(&rw->main_cond, &rw->mutex);
        }
        rw->readers++;
        pthread_mutex_unlock(&rw->mutex);
        return;
    }
    struct cajeta_fiber* self = __cajeta_current_fiber;
    pthread_mutex_lock(&rw->mutex);
    for (;;) {
        if (!rw->writer && rw->writers_waiting == 0) {
            rw->readers++;
            pthread_mutex_unlock(&rw->mutex);
            return;
        }
        self->next = NULL;
        if (rw->rwait_tail) {
            rw->rwait_tail->next = self;
            rw->rwait_tail = self;
        } else {
            rw->rwait_head = self;
            rw->rwait_tail = self;
        }
        self->state = CAJETA_FIBER_PARKED;
        pthread_mutex_unlock(&rw->mutex);
        __cajeta_swapcontext(&self->ctx, &__cajeta_carrier_ctx);
        pthread_mutex_lock(&rw->mutex);
    }
}

void __cajeta_rwlock_wrlock(void* p) {
    if (!p) return;
    struct cajeta_async_rwlock* rw = (struct cajeta_async_rwlock*) p;
    if (!__cajeta_current_fiber) {
        pthread_mutex_lock(&rw->mutex);
        rw->writers_waiting++;
        while (rw->writer || rw->readers > 0) {
            pthread_cond_wait(&rw->main_cond, &rw->mutex);
        }
        rw->writers_waiting--;
        rw->writer = 1;
        pthread_mutex_unlock(&rw->mutex);
        return;
    }
    struct cajeta_fiber* self = __cajeta_current_fiber;
    pthread_mutex_lock(&rw->mutex);
    rw->writers_waiting++;
    for (;;) {
        if (!rw->writer && rw->readers == 0) {
            rw->writers_waiting--;
            rw->writer = 1;
            pthread_mutex_unlock(&rw->mutex);
            return;
        }
        self->next = NULL;
        if (rw->wwait_tail) {
            rw->wwait_tail->next = self;
            rw->wwait_tail = self;
        } else {
            rw->wwait_head = self;
            rw->wwait_tail = self;
        }
        self->state = CAJETA_FIBER_PARKED;
        pthread_mutex_unlock(&rw->mutex);
        __cajeta_swapcontext(&self->ctx, &__cajeta_carrier_ctx);
        pthread_mutex_lock(&rw->mutex);
    }
}

// Detach both wait queues (writers first) and wake them once unlocked, so
// woken fibers re-check their predicate against the new state.
static void __cajeta_rwlock_wake_all_locked(struct cajeta_async_rwlock* rw,
                                            struct cajeta_fiber** ww,
                                            struct cajeta_fiber** rwq) {
    *ww = rw->wwait_head;
    rw->wwait_head = NULL;
    rw->wwait_tail = NULL;
    *rwq = rw->rwait_head;
    rw->rwait_head = NULL;
    rw->rwait_tail = NULL;
    pthread_cond_broadcast(&rw->main_cond);
}

void __cajeta_rwlock_rdunlock(void* p) {
    if (!p) return;
    struct cajeta_async_rwlock* rw = (struct cajeta_async_rwlock*) p;
    struct cajeta_fiber* ww = NULL;
    struct cajeta_fiber* rwq = NULL;
    pthread_mutex_lock(&rw->mutex);
    if (rw->readers > 0) rw->readers--;
    // Only the last reader out can let a writer in; wake then.
    if (rw->readers == 0) {
        __cajeta_rwlock_wake_all_locked(rw, &ww, &rwq);
    }
    pthread_mutex_unlock(&rw->mutex);
    __cajeta_ready_enqueue_list(ww);
    __cajeta_ready_enqueue_list(rwq);
}

void __cajeta_rwlock_wrunlock(void* p) {
    if (!p) return;
    struct cajeta_async_rwlock* rw = (struct cajeta_async_rwlock*) p;
    struct cajeta_fiber* ww = NULL;
    struct cajeta_fiber* rwq = NULL;
    pthread_mutex_lock(&rw->mutex);
    rw->writer = 0;
    __cajeta_rwlock_wake_all_locked(rw, &ww, &rwq);
    pthread_mutex_unlock(&rw->mutex);
    __cajeta_ready_enqueue_list(ww);
    __cajeta_ready_enqueue_list(rwq);
}

void __cajeta_rwlock_destroy(void* p) {
    if (!p) return;
    struct cajeta_async_rwlock* rw = (struct cajeta_async_rwlock*) p;
    pthread_cond_destroy(&rw->main_cond);
    pthread_mutex_destroy(&rw->mutex);
    free(rw);
}

// --- atomic<T> backing storage (R8 Slice 1) ---------------------------------
//
// Atomic<T> classes (cajeta.threading.AtomicInt32 / AtomicInt64) own a heap-
// allocated word that the compiler-emitted inline LLVM atomic instructions
// (atomicrmw / cmpxchg / load atomic / store atomic) operate on directly. The
// runtime's role is just the alloc/free of the underlying cell — the
// arithmetic and ordering live in IR so the optimizer can reason about them
// (a runtime call would defeat the point of an atomic). Plain malloc/free is
// fine: these cells aren't tracked in the live-set (the owning Atomic<T>
// class's drop wrapper calls the destroy intrinsic, and the cell never
// outlives its owner).
int32_t* __cajeta_atomic_i32_new(int32_t initial) {
    int32_t* cell = (int32_t*) malloc(sizeof(int32_t));
    if (!cell) {
        fprintf(stderr, "cajeta: __cajeta_atomic_i32_new failed\n");
        abort();
    }
    // Initial store is seq_cst so a subsequent reader on another carrier
    // sees the constructed value without an extra fence at the call site.
    __atomic_store_n(cell, initial, __ATOMIC_SEQ_CST);
    return cell;
}

void __cajeta_atomic_i32_destroy(int32_t* cell) {
    if (cell) free(cell);
}

int64_t* __cajeta_atomic_i64_new(int64_t initial) {
    int64_t* cell = (int64_t*) malloc(sizeof(int64_t));
    if (!cell) {
        fprintf(stderr, "cajeta: __cajeta_atomic_i64_new failed\n");
        abort();
    }
    __atomic_store_n(cell, initial, __ATOMIC_SEQ_CST);
    return cell;
}

void __cajeta_atomic_i64_destroy(int64_t* cell) {
    if (cell) free(cell);
}

// Abort with a diagnostic when an array index is out of bounds. Compiler emits a
// conditional branch to this from ArrayIndexExpression when bounds checking is on.
void __cajeta_array_bounds_fail(int64_t index, int64_t dim) {
    // write(2) rather than fprintf(stderr): abort() does not flush stdio, and
    // on Windows stderr is block-buffered when piped (e.g. under a gtest death
    // test), so an fprintf'd message is lost — the diagnostic must reach the
    // fd directly. snprintf into a stack buffer first (no FILE* involved).
    char buf[160];
    int n = snprintf(buf, sizeof(buf),
                     "cajeta: array index %lld out of bounds for dimension size %lld\n",
                     (long long) index, (long long) dim);
    if (n > 0) {
        if (n > (int) sizeof(buf)) n = (int) sizeof(buf);
        (void) write(2, buf, (size_t) n);
    }
    abort();
}

// --- exception handling (setjmp/longjmp-based) -------------------------------
//
// Each try-block allocates a `cajeta_exception_frame` on the stack and registers
// it with __cajeta_exc_push. `throw` writes the value into the topmost frame and
// longjmps back to its setjmp point. The compiler emits the setjmp call inline
// (it must run in the caller's frame), so the runtime never sees it directly.
//
// Per-thread: each OS thread has its own `__cajeta_main_exc_top` __thread
// slot; each carrier-hosted fiber owns its own slot inside `cajeta_fiber`.
// `__cajeta_exc_top_ptr` picks the right one based on whether the call
// site is running inside a fiber. The drop-chain head uses the same model.

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

// Debug-mode variant — extends the base entry with source-position
// tags (cajeta-docs/CompilerModes.md § Source-tagged drop-chain
// entries). The first four fields share the base entry's layout, so
// a debug entry passed to pop_run (which only touches obj/drop_fn/
// prev/active) works without a separate pop helper.
//
// The compiler emits debug entries when CompilerFlags::sourceTags is
// on, allocating CAJETA_DROP_ENTRY_BYTES_DEBUG (40) instead of the
// release-mode 32, and calls __cajeta_drop_push_debug to populate
// the tag fields. SIGABRT handler / diagnostic dumps read the tags
// via the helpers below.
struct cajeta_drop_entry_debug {
    void* obj;                                  // +0
    void (*drop_fn)(void*);                     // +8
    struct cajeta_drop_entry* prev;             // +16  (same shape as base)
    int8_t active;                              // +24
    int8_t _pad[3];                             // +25
    int32_t alloc_line;                         // +28
    const char* alloc_file;                     // +32
    /* total: 40 bytes */
};

size_t __cajeta_drop_entry_size(void) {
    return sizeof(struct cajeta_drop_entry);
}

size_t __cajeta_drop_entry_size_debug(void) {
    return sizeof(struct cajeta_drop_entry_debug);
}

// Drop chain head — per-thread (main has its own __thread slot; carrier-
// hosted fibers each own a slot inside their cajeta_fiber struct). Before
// this rollout this was a single global static. With the carrier thread
// running concurrently with main on shared globals, even a "single-
// carrier" model races on the head pointer. Promoting to per-thread/
// per-fiber is the doc's planned shape (AsyncStatus.md § TLS-promote
// the exception chain + drop-chain head).
static __thread struct cajeta_drop_entry* __cajeta_main_drop_top = NULL;

// Returns a pointer to the current drop_top slot — either the running
// fiber's slot or the main thread's TLS slot. Callers read or write
// through this pointer, so push/pop works uniformly regardless of
// fiber vs main context. Mirrors __cajeta_scope_top_ptr.
static struct cajeta_drop_entry** __cajeta_drop_top_ptr(void) {
    if (__cajeta_current_fiber) {
        return &__cajeta_current_fiber->drop_top;
    }
    return &__cajeta_main_drop_top;
}

// Observability for tests: bumped every time a drop function actually fires
// (i.e. an active entry pops or unwinds). Tests can read it to assert that
// owned resources are freed at expected program points. Atomic so drops
// fired on the carrier thread are visible to tests reading from main.
static int64_t __cajeta_drop_count = 0;

int64_t __cajeta_drop_count_get(void) {
    return __atomic_load_n(&__cajeta_drop_count, __ATOMIC_SEQ_CST);
}
void __cajeta_drop_count_reset(void) {
    __atomic_store_n(&__cajeta_drop_count, 0, __ATOMIC_SEQ_CST);
}

// ----------------------------------------------------------------------------
// Drop-chain validation (CompilerModes.md § --drop-chain-validate).
//
// When enabled, runtime invariants on the drop chain are checked at every
// push / pop / mark_inactive transition. Violations dump the chain to
// stderr with a labeled error code and abort — the same fail-loud rule the
// glibc heap-corruption path uses. Caught corruption modes:
//
//   - **Push-onto-self.** Caller hands push() an entry pointer equal to
//     the current top — would create an immediate self-cycle (e->prev = e)
//     and walks would loop forever. Almost always a compiler-codegen bug
//     where two distinct locals share the same entry slot.
//   - **Pop with mismatched top.** Caller passes entry `e` to pop_run but
//     `*top != e` — out-of-order pop, double-pop, or chain reordering. The
//     LIFO discipline is load-bearing for unwinding to work, so this is a
//     hard error rather than a soft skip.
//   - **`active` neither 0 nor 1.** Bit-rot somewhere — uninitialized stack
//     reuse, wild store, etc. Surfaces before we'd otherwise misread it
//     as "skip drop" or "fire drop".
//
// Flag default is OFF (release-mode and the existing-behavior contract for
// the pre-instrumented chain). JIT init flips it on per-test from
// Options.dropChainValidateEnabled.
// ----------------------------------------------------------------------------
static int __cajeta_drop_chain_validate_enabled = 0;

void __cajeta_set_drop_chain_validate(int enabled) {
    __cajeta_drop_chain_validate_enabled = enabled ? 1 : 0;
}

int __cajeta_get_drop_chain_validate(void) {
    return __cajeta_drop_chain_validate_enabled;
}

// Forward decl — the dumper lives below the push helpers.
int32_t __cajeta_dump_drop_chain(void);

static void __cajeta_drop_chain_corruption(const char* code, const char* what) {
    fprintf(stderr,
        "cajeta: drop chain corruption detected (%s): %s\n",
        code, what);
    __cajeta_dump_drop_chain();
    abort();
}

// Push an owner onto the drop chain. The entry storage is stack-allocated in
// the caller's frame; we never own the memory, only chain pointers through it.
void __cajeta_drop_push(struct cajeta_drop_entry* e, void* obj, void (*drop_fn)(void*)) {
    struct cajeta_drop_entry** top = __cajeta_drop_top_ptr();
    if (__cajeta_drop_chain_validate_enabled) {
        if (e == *top && *top != NULL) {
            __cajeta_drop_chain_corruption(
                "CAJETA_ERROR_DROP_CHAIN_SELF_PUSH",
                "push entry pointer equals current top (would self-cycle)");
        }
        if (e == NULL) {
            __cajeta_drop_chain_corruption(
                "CAJETA_ERROR_DROP_CHAIN_NULL_PUSH",
                "push called with NULL entry pointer");
        }
    }
    e->obj = obj;
    e->drop_fn = drop_fn;
    e->prev = *top;
    e->active = 1;
    *top = e;
}

// Tracks whether any debug-shape entry has been pushed in this process.
// Flipped by __cajeta_drop_push_debug; read by the SIGABRT handler so it
// knows whether reading the extended (alloc_file, alloc_line) fields is
// safe. Release-mode builds never call push_debug, so this stays 0 and
// the handler dumps just the base fields.
static int __cajeta_has_debug_entries = 0;

// Debug variant — same wiring as __cajeta_drop_push, plus alloc-site source
// tags written into the extended cajeta_drop_entry_debug shape. Compiler
// emits a call to this helper instead of the release variant when
// CompilerFlags::sourceTags is on. Pop unchanged (uses the base layout's
// active + prev offsets which match).
void __cajeta_drop_push_debug(struct cajeta_drop_entry_debug* e, void* obj,
                              void (*drop_fn)(void*),
                              const char* alloc_file, int32_t alloc_line) {
    struct cajeta_drop_entry** top = __cajeta_drop_top_ptr();
    if (__cajeta_drop_chain_validate_enabled) {
        if ((struct cajeta_drop_entry*) e == *top && *top != NULL) {
            __cajeta_drop_chain_corruption(
                "CAJETA_ERROR_DROP_CHAIN_SELF_PUSH",
                "push (debug) entry pointer equals current top (would self-cycle)");
        }
        if (e == NULL) {
            __cajeta_drop_chain_corruption(
                "CAJETA_ERROR_DROP_CHAIN_NULL_PUSH",
                "push (debug) called with NULL entry pointer");
        }
    }
    e->obj = obj;
    e->drop_fn = drop_fn;
    e->prev = *top;
    e->active = 1;
    e->_pad[0] = e->_pad[1] = e->_pad[2] = 0;
    e->alloc_line = alloc_line;
    e->alloc_file = alloc_file;
    *top = (struct cajeta_drop_entry*) e;
    __cajeta_has_debug_entries = 1;
}

// Diagnostic accessors used by the SIGABRT handler and by tests. Reads the
// debug-shape extended fields from an entry. Caller is responsible for
// passing a pointer to a debug-shape entry — the compiler only emits
// debug entries when sourceTags is on, so within a debug-mode build
// every chain entry is debug-shape. Mixed-shape chains are out of scope
// for v1.
const char* __cajeta_drop_chain_head_alloc_file(void) {
    struct cajeta_drop_entry* head = *__cajeta_drop_top_ptr();
    if (!head) return NULL;
    return ((struct cajeta_drop_entry_debug*) head)->alloc_file;
}

int32_t __cajeta_drop_chain_head_alloc_line(void) {
    struct cajeta_drop_entry* head = *__cajeta_drop_top_ptr();
    if (!head) return 0;
    return ((struct cajeta_drop_entry_debug*) head)->alloc_line;
}

// Walk the per-thread drop chain and print each entry to stderr. Returns
// the number of entries dumped. Reads alloc tags from each entry when
// __cajeta_has_debug_entries is set (i.e. push_debug was used at least
// once in this process); otherwise prints just the base fields.
//
// Capped at MAX_ENTRIES so a runaway / corrupt chain doesn't hang the
// abort path. SIGABRT handler calls this; tests may also call it through
// Cajeta.dumpDropChain() to verify the dump shape without aborting.
//
// Not signal-safe (fprintf), but the SIGABRT context — typically glibc
// detecting heap corruption — already uses fprintf in __libc_message, so
// the loose-signal-safety pragmatism is consistent with the platform's
// own abort path. If this bites in practice, replace with write(2) +
// snprintf'd buffer.
int32_t __cajeta_dump_drop_chain(void) {
    const int MAX_ENTRIES = 32;
    struct cajeta_drop_entry** top = __cajeta_drop_top_ptr();
    struct cajeta_drop_entry* e = *top;
    int count = 0;
    fprintf(stderr, "cajeta: drop chain (head first, %d entries max shown):\n",
            MAX_ENTRIES);
    while (e && count < MAX_ENTRIES) {
        if (__cajeta_has_debug_entries) {
            struct cajeta_drop_entry_debug* d = (struct cajeta_drop_entry_debug*) e;
            fprintf(stderr,
                "  [%d] obj=%p drop_fn=%p active=%d   alloc=%s:%d\n",
                count, e->obj, (void*) e->drop_fn, (int) e->active,
                d->alloc_file ? d->alloc_file : "(null)",
                (int) d->alloc_line);
        } else {
            fprintf(stderr,
                "  [%d] obj=%p drop_fn=%p active=%d\n",
                count, e->obj, (void*) e->drop_fn, (int) e->active);
        }
        e = e->prev;
        count++;
    }
    if (e) {
        fprintf(stderr,
            "  ... (more entries; cap reached, raise MAX_ENTRIES to see them)\n");
    }
    return count;
}

// SIGABRT handler — installed by the runtime constructor below. On abort
// (typically glibc heap-corruption detection), dump the drop chain to
// stderr with source tags when available, then chain to the previous
// handler so the abort still kills the process.
//
// POSIX path uses sigaction (richer 3-arg handler with siginfo + ucontext).
// Windows / MinGW doesn't provide sigaction; fall back to the basic
// signal(SIGABRT, ...) C89 surface. We lose the siginfo + ucontext
// payload but the abort-and-dump-drop-chain still fires.
#if defined(_WIN32)

static void (*__cajeta_prev_sigabrt)(int) = NULL;

static void __cajeta_sigabrt_handler(int signo) {
    fprintf(stderr,
        "\ncajeta: SIGABRT caught — likely heap corruption or assertion.\n");
    __cajeta_dump_drop_chain();
    if (__cajeta_prev_sigabrt && __cajeta_prev_sigabrt != SIG_DFL
            && __cajeta_prev_sigabrt != SIG_IGN) {
        __cajeta_prev_sigabrt(signo);
        return;
    }
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}

void __cajeta_install_sigabrt_handler(void) {
    // No "is something already installed?" probe on Windows — `signal()`
    // returns the previous handler when setting a new one, no
    // distinction between SIG_DFL and "user-installed but unknown."
    // Just install; the host's static copy fires before any JIT module
    // loads anyway.
    void (*prev)(int) = signal(SIGABRT, __cajeta_sigabrt_handler);
    if (prev != SIG_ERR) {
        __cajeta_prev_sigabrt = prev;
    }
}

#else

static struct sigaction __cajeta_prev_sigabrt;

static void __cajeta_sigabrt_handler(int signo, siginfo_t* info, void* uctx) {
    fprintf(stderr,
        "\ncajeta: SIGABRT caught — likely heap corruption or assertion.\n");
    __cajeta_dump_drop_chain();
    // Chain to the previous handler so the process still dies. If there
    // wasn't one (or it was SIG_DFL/SIG_IGN), restore the default and
    // re-raise.
    if (__cajeta_prev_sigabrt.sa_flags & SA_SIGINFO) {
        if (__cajeta_prev_sigabrt.sa_sigaction) {
            __cajeta_prev_sigabrt.sa_sigaction(signo, info, uctx);
            return;
        }
    } else if (__cajeta_prev_sigabrt.sa_handler != SIG_DFL
               && __cajeta_prev_sigabrt.sa_handler != SIG_IGN
               && __cajeta_prev_sigabrt.sa_handler != NULL) {
        __cajeta_prev_sigabrt.sa_handler(signo);
        return;
    }
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}

void __cajeta_install_sigabrt_handler(void) {
    // Skip if SIGABRT already has a non-default handler. Each JIT-loaded
    // copy of the runtime would otherwise re-install on its own
    // `__attribute__((constructor))`, chaining the prior handler in as
    // "previous". When the prior JIT module later unmaps, its handler
    // function lives in freed memory — a subsequent SIGABRT jumps into
    // that unmapped region and the process dies with SIGSEGV instead of
    // SIGABRT (breaks death tests, obscures real bugs).
    //
    // Each JIT module has its own copy of `__cajeta_sigabrt_handler`, so
    // a pointer-equality check against this module's copy wouldn't catch
    // a prior install from another module. The conservative rule is:
    // if anything other than SIG_DFL/SIG_IGN is installed, leave it
    // alone — the host's static copy already wired the handler at
    // process load (constructor runs before main), and that's the
    // version backed by lifetime-stable code.
    struct sigaction cur;
    if (sigaction(SIGABRT, NULL, &cur) == 0) {
        bool already =
            (cur.sa_flags & SA_SIGINFO)
                ? (cur.sa_sigaction != NULL)
                : (cur.sa_handler != SIG_DFL && cur.sa_handler != SIG_IGN
                   && cur.sa_handler != NULL);
        if (already) return;
    }
    struct sigaction sa;
    sa.sa_sigaction = __cajeta_sigabrt_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGABRT, &sa, &__cajeta_prev_sigabrt);
}

#endif

// Auto-install at runtime load. Constructor runs before main(); the handler
// is then armed for the program lifetime, including before any Cajeta code
// has executed (so a stdlib-load-time abort is also caught).
__attribute__((constructor))
static void __cajeta_runtime_init(void) {
    __cajeta_install_sigabrt_handler();
}

// Pop the topmost entry and run its drop function if still active. Caller
// passes the entry pointer so popping can verify shape (in debug builds; v1
// trusts the caller).
void __cajeta_drop_pop_run(struct cajeta_drop_entry* e) {
    struct cajeta_drop_entry** top = __cajeta_drop_top_ptr();
    if (__cajeta_drop_chain_validate_enabled) {
        if (e == NULL) {
            __cajeta_drop_chain_corruption(
                "CAJETA_ERROR_DROP_CHAIN_NULL_POP",
                "pop_run called with NULL entry pointer");
        }
        if (*top != e) {
            __cajeta_drop_chain_corruption(
                "CAJETA_ERROR_DROP_CHAIN_POP_MISMATCH",
                "pop_run entry does not match chain top "
                "(out-of-order pop, double-pop, or chain reordering)");
        }
        if (e->active != 0 && e->active != 1) {
            __cajeta_drop_chain_corruption(
                "CAJETA_ERROR_DROP_CHAIN_BAD_ACTIVE",
                "entry active flag is neither 0 nor 1 (bit-rot)");
        }
    }
    if (e->active && e->drop_fn) {
        __atomic_fetch_add(&__cajeta_drop_count, 1, __ATOMIC_SEQ_CST);
        e->drop_fn(e->obj);
    }
    *top = e->prev;
}

// Mark an entry inactive (the owner has been moved out via `#`). The entry
// remains on the chain so scope-exit pop logic still finds it, but the drop
// function won't run.
void __cajeta_drop_mark_inactive(struct cajeta_drop_entry* e) {
    if (__cajeta_drop_chain_validate_enabled) {
        if (e == NULL) {
            __cajeta_drop_chain_corruption(
                "CAJETA_ERROR_DROP_CHAIN_NULL_MARK",
                "mark_inactive called with NULL entry pointer");
        }
        if (e->active != 0 && e->active != 1) {
            __cajeta_drop_chain_corruption(
                "CAJETA_ERROR_DROP_CHAIN_BAD_ACTIVE",
                "mark_inactive on entry with bit-rotted active flag");
        }
    }
    e->active = 0;
}

// CP7-1c host accessor for the debug frame chain. Companion to the
// __cajeta_dbg_local_* accessors defined up near the frame-chain helpers, but
// placed here because it dereferences a cajeta_drop_entry (defined above; the
// frame-chain block stores the entry only as an opaque void*). Reports the
// live lifetime signal for local `i`: 1 = active owner (scheduled to drop),
// 0 = inactive (moved out at runtime), -1 = no drop entry (borrow / value).
// The `active` flag sits at the same offset in the base and debug entry
// shapes, so the base cast is valid for both. Pure read — safe to call from
// the debugger thread while parked (FR-2.3).
int8_t __cajeta_dbg_local_drop_active(void* frame, int i) {
    if (!frame) return -1;
    struct cajeta_dbg_frame* f = frame;
    if (i < 0 || i >= f->nlocals) return -1;
    void* e = f->locals[i].drop_entry;
    if (!e) return -1;
    return ((struct cajeta_drop_entry*) e)->active ? 1 : 0;
}


// S10.4 — kind-tag dispatched interface value drop. Called at scope exit
// for every interface-typed local. Reads the fat pointer's kind word
// and either invokes the underlying class's drop (OWNED_CLASS) or
// no-ops (BORROWED_*). Mirrors the layout established in S9.5.1 and the
// per-(impl, iface) vtable convention from S10.4: vtable slot 0 holds
// the implementer's drop function; method entries start at slot 1.
//
// Layout reminder:
//   body + 0  = data_ptr
//   body + 8  = vtable_ptr  (vtable[0] = drop_fn, vtable[1..N] = methods)
//   body + 16 = kind (i64)
void __cajeta_iface_drop(void* body) {
    if (!body) return;
    void** words = (void**) body;
    void* data_ptr = words[0];
    void** vtable = (void**) words[1];
    int64_t kind = *((int64_t*) (((char*) body) + 16));
    if (kind == 1 /* IFACE_KIND_OWNED_CLASS */) {
        if (vtable && data_ptr) {
            void (*drop_fn)(void*) = (void (*)(void*)) vtable[0];
            if (drop_fn) drop_fn(data_ptr);
        }
    }
    /* BORROWED_CLASS (0) / BORROWED_STRUCT (2) — no-op. The
     * underlying class lifetime is owned by another holder (DI cache,
     * class field, etc.) and the struct body is owned by its source
     * local's own drop entry. */
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
// VTable byte layout (kept in sync with StructureMetadata::createVirtualTableType):
//   [0..1]   i16 version
//   [2..3]   i16 count
//   [4..7]   pad (LLVM auto-inserts to align ptr to 8 bytes)
//   [8..15]  ptr parent_vtable        (NULL at root)
//   [16..23] ptr drop_fn              (this class's synthesized drop wrapper —
//                                      __cajeta_class_virtual_drop loads this
//                                      to route drops through the dynamic type)
//   [24..]   [count x { i64 hash, ptr fn }] entries
#define CAJETA_VTABLE_PARENT_OFFSET 8
#define CAJETA_VTABLE_DROP_FN_OFFSET 16
#define CAJETA_VTABLE_ENTRIES_OFFSET 24

// Gap-1 fix — virtual dispatch on drop. Heap class locals push this as
// their drop fn (in place of the static per-class drop wrapper). At
// fire time we load the instance's vtable pointer (slot 0 of the
// instance body) and call through the drop_fn slot in the vtable
// header (CAJETA_VTABLE_DROP_FN_OFFSET) — which routes to the dynamic
// type's destructor regardless of the declared type of the binding.
// Without this, `Animal a = heap Dog()` at scope exit calls
// __cajeta_test_Animal_drop (statically bound at the push site),
// skipping ~Dog().
//
// instance layout (class body):
//   instance[0] = vtable_ptr → CAJETA_VTABLE_DROP_FN_OFFSET into that
//                 vtable global holds this class's heap-drop wrapper.
void __cajeta_class_virtual_drop(void* instance) {
    if (!instance) return;
    // Idempotent claim — FieldOwnership.md § Solution B. Auto field drop
    // and the owning local's chain pop both route through here for the
    // same address (e.g. Optional<Hello>.value aliases a heap Hello
    // local). First caller wins the live-set claim and runs the
    // destructor + free; second caller no-ops without re-running the
    // user's ~Class() body.
    if (!__cajeta_live_set_claim(instance)) return;
    void* vptr = *(void**) instance;
    if (!vptr) return;
    void (*drop_fn)(void*) =
        *(void (**)(void*)) ((char*) vptr + CAJETA_VTABLE_DROP_FN_OFFSET);
    if (drop_fn) drop_fn(instance);
}

void* __cajeta_vtable_lookup(void* vptr, int64_t hash) {
    if (!vptr) return NULL;
    const int16_t* hdr = (const int16_t*) vptr;
    int32_t count = hdr[1];                 // slot 1 = count
    if (count <= 0) return NULL;
    const struct cajeta_vtable_entry* entries =
        (const struct cajeta_vtable_entry*)
            ((const char*) vptr + CAJETA_VTABLE_ENTRIES_OFFSET);
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

// UnrecoverableException's vtable address, published by codegen.
// __cajeta_is_unrecoverable compares each ancestor vtable against this.
//
// Codegen (Compiler::emitUnrecoverableMarker) emits a module global ctor
// that calls __cajeta_set_unrecoverable_vtable with
// cajeta.lang.UnrecoverableException#VTable. A ctor + plain runtime call
// resolves identically on ELF/MachO/COFF and in both JIT (LLJIT runs
// llvm.global_ctors at initialize()) and AOT (the C runtime runs them
// before main) — unlike the previous weak-global-override scheme, which
// only bound under ELF and left JIT detection reading NULL on MachO/COFF.
static void* g_unrecoverable_vtable = NULL;

void __cajeta_set_unrecoverable_vtable(void* vtable) {
    g_unrecoverable_vtable = vtable;
}

// Walk a Throwable's vtable chain to determine whether it's an
// UnrecoverableException (or any descendant thereof). Returns 1 if so,
// 0 otherwise. Driven by the parent_vtable pointer at offset 8 of each
// vtable global; walks until either a match is found or the chain hits
// NULL (root).
int32_t __cajeta_is_unrecoverable(void* throwable) {
    if (!throwable) return 0;
    // Defensive: the legacy `throw 42` idiom IntToPtrs an integer into
    // the runtime; the resulting "pointer" is the integer's bit pattern
    // and is virtually never a real heap address. Dereferencing it for
    // the vtable read would SIGSEGV. Filter anything below the
    // typical zero-page boundary so we treat legacy int throws as
    // (definitely-not-)Unrecoverable. Long-term: phase out int throws
    // in favor of real Throwable instances.
    if ((uintptr_t) throwable < 4096) return 0;
    void* vtable = *(void**) throwable;   // instance slot 0 = vtable ptr
    if (!g_unrecoverable_vtable) return 0;
    while (vtable) {
        if (vtable == g_unrecoverable_vtable) return 1;
        vtable = *(void**) ((char*) vtable + CAJETA_VTABLE_PARENT_OFFSET);
    }
    return 0;
}

// __cajeta_exc_matches — does the thrown object's runtime type match (is-a)
// the catch clause's declared type? Generalizes __cajeta_is_unrecoverable: the
// caller passes the catch type's #VTable global; we walk the thrown object's
// vtable parent chain (parent at CAJETA_VTABLE_PARENT_OFFSET, the same chain the
// unrecoverable check uses) and return 1 iff `catch_vtable` appears anywhere in
// it — i.e. the thrown class IS the catch class or a descendant of it. This is
// the runtime half of try/catch type dispatch (TryStatement emits one call per
// catch clause, in source order, first match wins). A null `catch_vtable`
// (a non-class / catch-all clause) is handled at the codegen level, not here.
int32_t __cajeta_exc_matches(void* throwable, void* catch_vtable) {
    if (!throwable || !catch_vtable) return 0;
    // Same low-address guard as the unrecoverable walk: a legacy `throw 42`
    // int-as-pointer must never be dereferenced for its vtable slot.
    if ((uintptr_t) throwable < 4096) return 0;
    void* vtable = *(void**) throwable;   // instance slot 0 = vtable ptr
    // Defensive walk: cap the depth and sanity-check each vtable pointer is a
    // real (high) address before dereferencing its parent slot. A malformed or
    // uninitialized chain returns no-match rather than segfaulting the matcher.
    for (int depth = 0; depth < 256; ++depth) {
        if ((uintptr_t) vtable < 4096) break;
        if (vtable == catch_vtable) return 1;
        vtable = *(void**) ((char*) vtable + CAJETA_VTABLE_PARENT_OFFSET);
    }
    return 0;
}

// Forward decl — defined alongside __cajeta_throw further down.
static void __cajeta_emit_uncaught(void* value, int is_unrec);

// Called by the fiber trampoline's catch block (per Error-model #205 and
// #210). If the thrown value is an Unrecoverable, print + abort the
// whole process — propagating into the Task slot would let a runtime
// invariant violation hide behind await suspension. Recoverable returns
// normally; the trampoline's caller stores it on the Task's exception
// slot for await to re-raise.
void __cajeta_fiber_handle_throw(void* thrown) {
    if (__cajeta_is_unrecoverable(thrown)) {
        __cajeta_emit_uncaught(thrown, /*is_unrec=*/1);
        abort();
    }
}

struct cajeta_exception_frame {
    jmp_buf buf;
    struct cajeta_exception_frame* prev;
    // R5/Error-model #202: the thrown value is now a void* — typed at the
    // codegen level as a Throwable*, but the runtime is type-agnostic so we
    // store it as a bare pointer. Backwards-compatible with the old int64-
    // throw idiom: ThrowStatement converts integer literals via IntToPtr,
    // TryStatement's catch binding reads back via PtrToInt when the
    // declared catch type is integer-shaped.
    void* thrown_value;
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

// Exception chain head — per-thread (main has its own __thread slot;
// carrier-hosted fibers each own a slot inside their cajeta_fiber
// struct). Same rationale as the drop chain head above. The
// drop_watermark stored in each frame snapshots the per-thread drop
// top at try-entry time, so an unwind through __cajeta_throw works
// against the same chain the user code pushed into.
static __thread struct cajeta_exception_frame* __cajeta_main_exc_top = NULL;

static struct cajeta_exception_frame** __cajeta_exc_top_ptr(void) {
    if (__cajeta_current_fiber) {
        return &__cajeta_current_fiber->exc_top;
    }
    return &__cajeta_main_exc_top;
}

void __cajeta_exc_push(struct cajeta_exception_frame* f) {
    struct cajeta_exception_frame** top = __cajeta_exc_top_ptr();
    struct cajeta_drop_entry** dropTop = __cajeta_drop_top_ptr();
    f->prev = *top;
    f->thrown_value = NULL;
    // Snapshot the current drop-chain top so a throw can unwind back to here.
    f->drop_watermark = *dropTop;
    *top = f;
}

void __cajeta_exc_pop(void) {
    struct cajeta_exception_frame** top = __cajeta_exc_top_ptr();
    if (*top) {
        *top = (*top)->prev;
    }
}

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

// --- cajeta.time.ZoneId tz-database lookup -------------------------------
//
// Resolves the UTC offset (seconds) for an IANA region zone (e.g.
// "America/Los_Angeles") at a given epoch second, reading the system tz
// database at /usr/share/zoneinfo and honoring DST transitions. Returns
// INT32_MIN when the zone can't be resolved (unknown name, unreadable file,
// malformed TZif) so the cajeta side can throw; "UTC"/"GMT"/"Z"/"Etc/UTC"
// resolve to 0 without touching the filesystem (the static-build fallback).
//
// TZif format: RFC 8536 / tzfile(5). We prefer the version-2/3 64-bit data
// block (correct past 2038); a bare version-1 file falls back to 32-bit.

static int32_t cj_tzif_be32(const uint8_t* p) {
    return (int32_t) (((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
                      ((uint32_t) p[2] << 8) | (uint32_t) p[3]);
}

static int64_t cj_tzif_be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | (uint64_t) p[i];
    return (int64_t) v;
}

// Find the utoff (seconds) active at `epoch` within one TZif data block.
// `start` is the block's first byte (transition times). `timesize` is 4 or 8.
// All region accesses are bounds-checked against `fsz`; returns INT32_MIN on
// any inconsistency.
static int32_t cj_tzif_offset_at(const uint8_t* buf, size_t fsz, size_t start,
                                 int timesize, uint32_t timecnt, uint32_t typecnt,
                                 int64_t epoch) {
    if (typecnt == 0) return INT32_MIN;
    size_t times = start;
    size_t idxs = times + (size_t) timecnt * (size_t) timesize;
    size_t ttinfo = idxs + (size_t) timecnt;          // typecnt * 6 bytes
    size_t ttend = ttinfo + (size_t) typecnt * 6;
    if (ttend > fsz) return INT32_MIN;

    // Latest transition with time <= epoch (transitions are sorted ascending).
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
        // Before the first transition: first non-DST type, else type 0.
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

int32_t __cajeta_tz_offset(const void* name_hdr, int64_t name_len, int64_t epoch) {
    if (!name_hdr || name_len <= 0 || name_len > 255) return INT32_MIN;
    const char* name = (const char*) name_hdr + 8;

    // UTC-equivalent fast paths — no filesystem, so they work in static builds.
    if ((name_len == 3 && (memcmp(name, "UTC", 3) == 0 || memcmp(name, "GMT", 3) == 0)) ||
        (name_len == 1 && name[0] == 'Z') ||
        (name_len == 7 && memcmp(name, "Etc/UTC", 7) == 0) ||
        (name_len == 7 && memcmp(name, "Etc/GMT", 7) == 0)) {
        return 0;
    }

    // Reject path traversal / absolute names.
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
            // Second header + 64-bit data block.
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

// substring(begin, end) for the LEGACY primitive-alias String path
// (i8* null-terminated C-strings). Half-open like Java's; out-of-range
// indices clamp; result is a freshly malloc'd null-terminated copy.
//
// Note: cajeta.lang.String (the class form) substring is view-based
// per the never-drop rule (see cajeta-docs/stdlib/lang/String.md §
// "Substring + slicing"). This C-string variant still copies because
// the null-terminated ABI can't express a slice (a subspan of a longer
// string would continue to the original terminator). The class
// substring will be implemented in pure Cajeta as a view-mode String
// pointing into the parent's bytes — no malloc, no free.
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
// Implements the runtime hash primitives the language uses for Object.hash()
// and the cajeta.hash.* stdlib classes (see StandardLibrary.md §cajeta.hash
// and CajetaReflect.md "Performance"). Two algorithms cover the surface:
//
//   * SplitMix64 finalizer for primitive value hashing — int64.hash(),
//     int32.hash(), float64.hash(), float32.hash(), boolean.hash(),
//     pointer identity. Three multiplications + three XORs; well-tested
//     mixer (Java's SplittableRandom, Rust hashers, etc.). The per-process
//     seed is XOR'd in before mixing so two runs of the same program
//     produce different hash values (hash-flooding defense — attackers
//     can't predict bucket placement).
//
//   * XXH3-64 (scalar) for arbitrary byte buffers — backing for
//     cajeta.hash.XXHash3 and DefaultHasher. Multi-GB/s on modern CPUs.
//     Lands in a follow-up commit; this one ships the primitive-hash +
//     seed infrastructure first because HashMap<int64, V> and similar
//     primitive-keyed maps don't need it.
//
// The seed initializes once per process from /dev/urandom via a
// constructor function that fires before main(). Falls back to
// wall-clock + pid mixed through SplitMix64 if /dev/urandom isn't
// readable (sandboxes, embedded targets).

#include <fcntl.h>
#include <unistd.h>
#include <time.h>

// XXH_INLINE_ALL: the xxhash header ships in two modes — declare-only
// (link against libxxhash) and inline-all (full implementation in this
// translation unit). We pick inline-all so the runtime's bitcode + native
// build both carry the implementation; no separate libxxhash linkage step
// on the JIT or AOT side. -O2 dead-code-strips the unused XXH32/XXH64/
// XXH128 paths so binary growth is bounded to what we actually call.
#define XXH_INLINE_ALL
#include <xxhash.h>

static uint64_t __cajeta_hash_seed_value = 0;

__attribute__((constructor))
static void __cajeta_hash_seed_init(void) {
    uint64_t s = 0;
#if defined(_WIN32)
    // Windows: BCryptGenRandom (CNG) is the modern equivalent of
    // /dev/urandom — cryptographically strong, no /dev needed.
    // BCRYPT_USE_SYSTEM_PREFERRED_RNG saves us providing an algorithm
    // provider handle. Available since Vista.
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
    if (BCryptGenRandom(NULL, (PUCHAR) &s, sizeof(s),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 /* STATUS_SUCCESS */
            && s != 0) {
        __cajeta_hash_seed_value = s;
        return;
    }
#else
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, &s, sizeof(s));
        close(fd);
        if (n == (ssize_t) sizeof(s) && s != 0) {
            __cajeta_hash_seed_value = s;
            return;
        }
    }
#endif
    // Fallback: wall clock + pid mixed through SplitMix64. Lower-entropy
    // than /dev/urandom but still per-process-distinct and stable for
    // the lifetime of the process.
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t x = (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
#if defined(_WIN32)
    x ^= (uint64_t) GetCurrentProcessId() * 0x9E3779B97F4A7C15ULL;
#else
    x ^= (uint64_t) getpid() * 0x9E3779B97F4A7C15ULL;
#endif
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    __cajeta_hash_seed_value = x ? x : 0x9E3779B97F4A7C15ULL;
}

// Lazy seed accessor. The constructor function above pre-initializes
// the seed at process startup — but only in the native binary build
// of this file. JIT-loaded bitcode copies have a separate static
// __cajeta_hash_seed_value that the JIT doesn't auto-initialize (no
// .init_array invocation at module-load). Checking-and-initializing
// on first call keeps both paths correct. After the first call the
// branch is predicted-not-taken and folds away in hot code.
static inline uint64_t __cajeta_hash_seed_load(void) {
    uint64_t s = __cajeta_hash_seed_value;
    if (__builtin_expect(s == 0, 0)) {
        __cajeta_hash_seed_init();
        s = __cajeta_hash_seed_value;
    }
    return s;
}

// Exposed to user code as cajeta.hash.Hash.processSeed() — useful when
// caller-side hashing needs to align with the synthesized Object.hash()
// values (e.g. external hash table snapshot replay).
int64_t __cajeta_hash_seed(void) {
    return (int64_t) __cajeta_hash_seed_load();
}

// SplitMix64 finalizer — the mixer behind every primitive hash variant.
// Three multiplications + three XOR-shifts; passes SMHasher avalanche
// + bias + collision tests on its own.
static inline uint64_t splitmix64_finalize(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

int64_t __cajeta_hash_int64(int64_t value) {
    return (int64_t) splitmix64_finalize((uint64_t) value ^ __cajeta_hash_seed_load());
}

int64_t __cajeta_hash_int32(int32_t value) {
    // Sign-extend so all-ones int32 doesn't hash like ~0 int64 just by
    // happening to share the low bits.
    return (int64_t) splitmix64_finalize(
        (uint64_t) (int64_t) value ^ __cajeta_hash_seed_load());
}

int64_t __cajeta_hash_float64(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    // Canonicalize -0 to +0 — IEEE 754 says +0 == -0, so they must hash
    // identically. NaN ordering is unspecified by the standard; we hash
    // each distinct NaN bit pattern to a distinct value, which is what
    // serializers / HashMap callers usually want.
    if (bits == 0x8000000000000000ULL) bits = 0;
    return (int64_t) splitmix64_finalize(bits ^ __cajeta_hash_seed_load());
}

int64_t __cajeta_hash_float32(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    if (bits == 0x80000000U) bits = 0;
    return (int64_t) splitmix64_finalize((uint64_t) bits ^ __cajeta_hash_seed_load());
}

int64_t __cajeta_hash_boolean(int8_t value) {
    return (int64_t) splitmix64_finalize(
        (value ? 1ULL : 0ULL) ^ __cajeta_hash_seed_load());
}

// Pointer-identity hash. Used by IdentityHashMap, observer registries,
// weak-ref tables. Same mixer as the primitive variants so the
// distribution properties match.
int64_t __cajeta_hash_identity(void* p) {
    return (int64_t) splitmix64_finalize(
        (uint64_t)(uintptr_t) p ^ __cajeta_hash_seed_load());
}

// Combine two 64-bit hash values into one. Boost's hash_combine pattern
// adapted with the SplitMix mixer at the end. Used by manual hash()
// overrides that thread multiple field hashes together.
int64_t __cajeta_hash_combine(int64_t a, int64_t b) {
    uint64_t h = (uint64_t) a;
    h ^= (uint64_t) b + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return (int64_t) splitmix64_finalize(h);
}

// XXH3-64 over an arbitrary byte buffer. Backs cajeta.hash.XXHash3 and
// String.hash() (where String is a UTF-8 sequence). Per-process seed
// is mixed via XXH3's seed parameter — same hash-flooding defense
// the primitive variants get. Multi-GB/s on modern CPUs; small-input
// path (the typical field-hashing case) is a handful of cycles.
int64_t __cajeta_hash_bytes(const uint8_t* data, int64_t len) {
    if (len < 0) len = 0;
    return (int64_t) XXH3_64bits_withSeed(
        data, (size_t) len, __cajeta_hash_seed_load());
}

// Same algorithm with caller-supplied seed. For cases where the seed
// is part of the input (snapshot replay, cross-process hash table
// rendezvous, deterministic-test contexts).
int64_t __cajeta_hash_bytes_seeded(const uint8_t* data, int64_t len, int64_t seed) {
    if (len < 0) len = 0;
    return (int64_t) XXH3_64bits_withSeed(
        data, (size_t) len, (uint64_t) seed);
}

// --- cajeta.hash.MD5 --------------------------------------------------------
// RFC 1321 MD5. Cryptographically broken — surfaced here for HTTP ETag,
// S3 Content-MD5, asset fingerprinting, cache-key derivation, database
// row fingerprinting. The full digest is 16 bytes; the streaming
// Hasher.finish() projection returns the first 8 bytes as little-endian
// int64 (matching how SipHash / XXH3 fold to int64). Callers that need
// the full 16-byte digest call MD5.hash(...) or MD5.hashHex(...).

struct cajeta_md5_state {
    uint32_t s[4];          // A, B, C, D
    uint64_t bits;          // total bytes hashed * 8
    uint8_t  buf[64];       // partial-block buffer
    int32_t  buf_len;
};

static const uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

static const uint32_t MD5_S[64] = {
    7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
    5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
    4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
    6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21
};

static inline uint32_t md5_rotl(uint32_t x, uint32_t n) {
    return (x << n) | (x >> (32u - n));
}

static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t M[16];
    for (int i = 0; i < 16; i++) {
        M[i] = ((uint32_t) block[i*4 + 0])
             | ((uint32_t) block[i*4 + 1] << 8)
             | ((uint32_t) block[i*4 + 2] << 16)
             | ((uint32_t) block[i*4 + 3] << 24);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16) {
            f = (b & c) | ((~b) & d);
            g = (uint32_t) i;
        } else if (i < 32) {
            f = (d & b) | ((~d) & c);
            g = (uint32_t)((5 * i + 1) % 16);
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (uint32_t)((3 * i + 5) % 16);
        } else {
            f = c ^ (b | (~d));
            g = (uint32_t)((7 * i) % 16);
        }
        uint32_t temp = d;
        d = c;
        c = b;
        b = b + md5_rotl(a + f + MD5_K[i] + M[g], MD5_S[i]);
        a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void md5_init(struct cajeta_md5_state* s) {
    s->s[0] = 0x67452301;
    s->s[1] = 0xefcdab89;
    s->s[2] = 0x98badcfe;
    s->s[3] = 0x10325476;
    s->bits = 0;
    s->buf_len = 0;
}

static void md5_update(struct cajeta_md5_state* s,
                       const uint8_t* data, size_t len) {
    s->bits += (uint64_t) len * 8u;
    while (len > 0) {
        size_t to_copy = (size_t) (64 - s->buf_len);
        if (to_copy > len) to_copy = len;
        memcpy(s->buf + s->buf_len, data, to_copy);
        s->buf_len += (int32_t) to_copy;
        data += to_copy;
        len  -= to_copy;
        if (s->buf_len == 64) {
            md5_transform(s->s, s->buf);
            s->buf_len = 0;
        }
    }
}

static void md5_finalize(struct cajeta_md5_state* s, uint8_t out[16]) {
    // Append 0x80, pad with zeros to 56 mod 64, append 8-byte
    // little-endian bit count, transform.
    s->buf[s->buf_len++] = 0x80;
    if (s->buf_len > 56) {
        memset(s->buf + s->buf_len, 0, (size_t)(64 - s->buf_len));
        md5_transform(s->s, s->buf);
        s->buf_len = 0;
    }
    memset(s->buf + s->buf_len, 0, (size_t)(56 - s->buf_len));
    for (int i = 0; i < 8; i++) {
        s->buf[56 + i] = (uint8_t)(s->bits >> (i * 8));
    }
    md5_transform(s->s, s->buf);
    for (int i = 0; i < 4; i++) {
        out[i*4 + 0] = (uint8_t)(s->s[i] >> 0);
        out[i*4 + 1] = (uint8_t)(s->s[i] >> 8);
        out[i*4 + 2] = (uint8_t)(s->s[i] >> 16);
        out[i*4 + 3] = (uint8_t)(s->s[i] >> 24);
    }
}

// --- MD5 C ABI bridges -----------------------------------------------------
// Streaming state — opaque to cajeta. Allocator + finalizer match the
// destructor / ctor pattern the cajeta MD5 class uses.

void* __cajeta_md5_alloc(void) {
    struct cajeta_md5_state* s = (struct cajeta_md5_state*) malloc(sizeof *s);
    if (!s) return NULL;
    md5_init(s);
    return s;
}

void __cajeta_md5_free(void* state) {
    if (state) free(state);
}

void __cajeta_md5_reset(void* state) {
    if (state) md5_init((struct cajeta_md5_state*) state);
}

// `data_hdr` is a cajeta int8[] header — { i64 count, [N x i8] data }.
// Caller passes the explicit `len` since data.count() isn't always
// known at the call site; the runtime reads bytes from offset 8.
void __cajeta_md5_update(void* state, const void* data_hdr, int64_t len) {
    if (!state || !data_hdr || len <= 0) return;
    const uint8_t* data = ((const uint8_t*) data_hdr) + 8;
    md5_update((struct cajeta_md5_state*) state, data, (size_t) len);
}

// out_hdr is a cajeta int8[16] header. Writes 16 bytes starting at
// offset 8. Caller is responsible for sizing the array correctly.
void __cajeta_md5_finalize_into(void* state, void* out_hdr) {
    if (!state || !out_hdr) return;
    uint8_t* out = ((uint8_t*) out_hdr) + 8;
    md5_finalize((struct cajeta_md5_state*) state, out);
}

// Width-named primitive folders. Each writes the value's
// little-endian byte representation. Hasher's contract pins width
// (`writeInt16(1)` and `writeInt32(1)` produce different digests),
// so doing this on the C side avoids per-call temporary array
// allocation on the cajeta side.
void __cajeta_md5_write_i8 (void* state, int8_t  v) {
    if (state) md5_update((struct cajeta_md5_state*) state, (const uint8_t*) &v, 1);
}
void __cajeta_md5_write_i16(void* state, int16_t v) {
    if (!state) return;
    uint8_t b[2] = { (uint8_t)(v), (uint8_t)(v >> 8) };
    md5_update((struct cajeta_md5_state*) state, b, 2);
}
void __cajeta_md5_write_i32(void* state, int32_t v) {
    if (!state) return;
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (i * 8));
    md5_update((struct cajeta_md5_state*) state, b, 4);
}
void __cajeta_md5_write_i64(void* state, int64_t v) {
    if (!state) return;
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (i * 8));
    md5_update((struct cajeta_md5_state*) state, b, 8);
}
void __cajeta_md5_write_f32(void* state, float v) {
    if (!state) return;
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    __cajeta_md5_write_i32(state, (int32_t) bits);
}
void __cajeta_md5_write_f64(void* state, double v) {
    if (!state) return;
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    __cajeta_md5_write_i64(state, (int64_t) bits);
}
void __cajeta_md5_write_bool(void* state, int8_t v) {
    __cajeta_md5_write_i8(state, v ? 1 : 0);
}

// finish() Hasher projection: return the first 8 bytes of the digest
// as a little-endian int64. Reads s[0] and s[1] in their post-finalize
// state. NB: this mutates the state (calls md5_finalize), so a second
// finish() returns garbage for the same state — Hasher.finish() is
// terminal by contract.
int64_t __cajeta_md5_finish_int64(void* state) {
    if (!state) return 0;
    uint8_t digest[16];
    md5_finalize((struct cajeta_md5_state*) state, digest);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t) digest[i]) << (i * 8);
    }
    return (int64_t) v;
}

// One-shot variants. Caller pre-allocates the output array on the
// cajeta side (since @Native return of int8[] isn't ABI-bridged in
// v1 — the live-set registration that `new int8[N]` performs doesn't
// flow through a returned-from-C array header). These helpers fill
// the caller's buffer at `out_hdr + 8`.
void __cajeta_md5_oneshot_into(const void* data_hdr, int64_t len, void* out_hdr) {
    if (!out_hdr) return;
    struct cajeta_md5_state s;
    md5_init(&s);
    if (data_hdr && len > 0) {
        md5_update(&s, ((const uint8_t*) data_hdr) + 8, (size_t) len);
    }
    md5_finalize(&s, ((uint8_t*) out_hdr) + 8);
}

// Lowercase hex digest into a caller-supplied int8[32] buffer. The
// cajeta MD5.hashHex wrapper allocates `new int8[32]`, calls this,
// then wraps the array in a String.
void __cajeta_md5_oneshot_hex_into(const void* data_hdr, int64_t len, void* out_hdr) {
    if (!out_hdr) return;
    struct cajeta_md5_state s;
    md5_init(&s);
    if (data_hdr && len > 0) {
        md5_update(&s, ((const uint8_t*) data_hdr) + 8, (size_t) len);
    }
    uint8_t digest[16];
    md5_finalize(&s, digest);
    static const char HEX[16] = "0123456789abcdef";
    uint8_t* out = ((uint8_t*) out_hdr) + 8;
    for (int i = 0; i < 16; i++) {
        out[i*2 + 0] = (uint8_t) HEX[(digest[i] >> 4) & 0xF];
        out[i*2 + 1] = (uint8_t) HEX[digest[i] & 0xF];
    }
}

// --- cajeta.hash.SHA-256 (NET-11.1, FIPS 180-4) ----------------------------
// Kept in its own reviewable source file and #included here so it rides the
// single-TU runtime -> bitcode -> embed build with NO CMake change (the build
// compiles ONLY cajeta_runtime.c to bitcode; sibling .c files must be textually
// included to be embedded + linker-merged into user modules).
#include "cajeta_sha256.c"

// --- cajeta.hash.SipHash (SipHash-2-4) -------------------------------------
// SipHash-2-4 over arbitrary bytes with a 128-bit key. Designed for
// hash-flooding resistance — exactly the right algorithm when keys
// come from untrusted input (HTTP request bodies, shared cache
// lookups). v1 ships streaming + one-shot; the in-process default
// hasher uses XXH3 instead because SipHash is much slower per byte
// (~2-3 GB/s vs XXH3's ~30 GB/s) — speed beats DoS resistance for
// the in-process internal-keys case.
//
// Reference: Aumasson + Bernstein "SipHash: a fast short-input PRF"
// 2012.

struct cajeta_siphash_state {
    uint64_t v0, v1, v2, v3;     // working state
    uint8_t  buf[8];             // partial-word buffer
    int32_t  buf_len;
    uint64_t total_bytes;
};

static inline uint64_t sip_rotl(uint64_t x, int n) {
    return (x << n) | (x >> (64 - n));
}

static inline void sip_round(uint64_t* v0, uint64_t* v1,
                             uint64_t* v2, uint64_t* v3) {
    *v0 += *v1; *v1 = sip_rotl(*v1, 13); *v1 ^= *v0; *v0 = sip_rotl(*v0, 32);
    *v2 += *v3; *v3 = sip_rotl(*v3, 16); *v3 ^= *v2;
    *v0 += *v3; *v3 = sip_rotl(*v3, 21); *v3 ^= *v0;
    *v2 += *v1; *v1 = sip_rotl(*v1, 17); *v1 ^= *v2; *v2 = sip_rotl(*v2, 32);
}

static inline uint64_t sip_load_le64(const uint8_t* p) {
    return ((uint64_t) p[0])
         | ((uint64_t) p[1] << 8)
         | ((uint64_t) p[2] << 16)
         | ((uint64_t) p[3] << 24)
         | ((uint64_t) p[4] << 32)
         | ((uint64_t) p[5] << 40)
         | ((uint64_t) p[6] << 48)
         | ((uint64_t) p[7] << 56);
}

static void siphash_init(struct cajeta_siphash_state* s,
                         uint64_t k0, uint64_t k1) {
    s->v0 = k0 ^ 0x736f6d6570736575ULL;
    s->v1 = k1 ^ 0x646f72616e646f6dULL;
    s->v2 = k0 ^ 0x6c7967656e657261ULL;
    s->v3 = k1 ^ 0x7465646279746573ULL;
    s->buf_len = 0;
    s->total_bytes = 0;
}

static void siphash_absorb_block(struct cajeta_siphash_state* s,
                                 const uint8_t* block) {
    uint64_t m = sip_load_le64(block);
    s->v3 ^= m;
    sip_round(&s->v0, &s->v1, &s->v2, &s->v3);
    sip_round(&s->v0, &s->v1, &s->v2, &s->v3);
    s->v0 ^= m;
}

static void siphash_update(struct cajeta_siphash_state* s,
                           const uint8_t* data, size_t len) {
    s->total_bytes += len;
    while (s->buf_len > 0 && len > 0) {
        size_t to_copy = (size_t)(8 - s->buf_len);
        if (to_copy > len) to_copy = len;
        memcpy(s->buf + s->buf_len, data, to_copy);
        s->buf_len += (int32_t) to_copy;
        data += to_copy; len -= to_copy;
        if (s->buf_len == 8) {
            siphash_absorb_block(s, s->buf);
            s->buf_len = 0;
        }
    }
    while (len >= 8) {
        siphash_absorb_block(s, data);
        data += 8; len -= 8;
    }
    if (len > 0) {
        memcpy(s->buf, data, len);
        s->buf_len = (int32_t) len;
    }
}

static uint64_t siphash_finalize(struct cajeta_siphash_state* s) {
    // Final block: remaining bytes + length-modulo-256 in top byte.
    uint8_t last[8] = {0};
    memcpy(last, s->buf, (size_t) s->buf_len);
    last[7] = (uint8_t)(s->total_bytes & 0xFF);
    uint64_t m = sip_load_le64(last);
    s->v3 ^= m;
    sip_round(&s->v0, &s->v1, &s->v2, &s->v3);
    sip_round(&s->v0, &s->v1, &s->v2, &s->v3);
    s->v0 ^= m;
    // Finalization rounds (4 for SipHash-2-4).
    s->v2 ^= 0xFF;
    for (int i = 0; i < 4; i++) {
        sip_round(&s->v0, &s->v1, &s->v2, &s->v3);
    }
    return s->v0 ^ s->v1 ^ s->v2 ^ s->v3;
}

// --- SipHash C ABI bridges -------------------------------------------------

void* __cajeta_siphash_alloc(int64_t k0, int64_t k1) {
    struct cajeta_siphash_state* s = (struct cajeta_siphash_state*) malloc(sizeof *s);
    if (!s) return NULL;
    siphash_init(s, (uint64_t) k0, (uint64_t) k1);
    return s;
}

void __cajeta_siphash_free(void* state) {
    if (state) free(state);
}

void __cajeta_siphash_reset(void* state, int64_t k0, int64_t k1) {
    if (state) siphash_init((struct cajeta_siphash_state*) state,
                            (uint64_t) k0, (uint64_t) k1);
}

void __cajeta_siphash_update(void* state, const void* data_hdr, int64_t len) {
    if (!state || !data_hdr || len <= 0) return;
    siphash_update((struct cajeta_siphash_state*) state,
                   ((const uint8_t*) data_hdr) + 8, (size_t) len);
}

// finish() Hasher projection — also the natural digest. SipHash is
// inherently 64-bit so finish() returns the full result.
int64_t __cajeta_siphash_finish(void* state) {
    if (!state) return 0;
    return (int64_t) siphash_finalize((struct cajeta_siphash_state*) state);
}

int64_t __cajeta_siphash_oneshot(const void* data_hdr, int64_t len,
                                 int64_t k0, int64_t k1) {
    struct cajeta_siphash_state s;
    siphash_init(&s, (uint64_t) k0, (uint64_t) k1);
    if (data_hdr && len > 0) {
        siphash_update(&s, ((const uint8_t*) data_hdr) + 8, (size_t) len);
    }
    return (int64_t) siphash_finalize(&s);
}

// Width-named SipHash folders — same shape as MD5's.
void __cajeta_siphash_write_i8(void* state, int8_t v) {
    if (state) siphash_update((struct cajeta_siphash_state*) state,
                              (const uint8_t*) &v, 1);
}
void __cajeta_siphash_write_i16(void* state, int16_t v) {
    if (!state) return;
    uint8_t b[2] = { (uint8_t) v, (uint8_t)(v >> 8) };
    siphash_update((struct cajeta_siphash_state*) state, b, 2);
}
void __cajeta_siphash_write_i32(void* state, int32_t v) {
    if (!state) return;
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (i * 8));
    siphash_update((struct cajeta_siphash_state*) state, b, 4);
}
void __cajeta_siphash_write_i64(void* state, int64_t v) {
    if (!state) return;
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (i * 8));
    siphash_update((struct cajeta_siphash_state*) state, b, 8);
}
void __cajeta_siphash_write_f32(void* state, float v) {
    if (!state) return;
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    __cajeta_siphash_write_i32(state, (int32_t) bits);
}
void __cajeta_siphash_write_f64(void* state, double v) {
    if (!state) return;
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    __cajeta_siphash_write_i64(state, (int64_t) bits);
}

// --- cajeta.hash.XXHash3 (XXH3-64) ----------------------------------------
// XXH3-64 from upstream xxhash (already included at the top of this
// file for __cajeta_hash_bytes). We expose alloc/update/digest
// here so the cajeta XXHash3 class has a stable opaque-pointer
// streaming surface; the algorithm is the same one Default Hasher /
// String.hash / @AutoHash all use under the hood.

void* __cajeta_xxh3_alloc(int64_t seed) {
    XXH3_state_t* s = XXH3_createState();
    if (!s) return NULL;
    XXH3_64bits_reset_withSeed(s, (XXH64_hash_t) seed);
    return s;
}

void __cajeta_xxh3_free(void* state) {
    if (state) XXH3_freeState((XXH3_state_t*) state);
}

void __cajeta_xxh3_reset(void* state, int64_t seed) {
    if (state) XXH3_64bits_reset_withSeed(
        (XXH3_state_t*) state, (XXH64_hash_t) seed);
}

void __cajeta_xxh3_update(void* state, const void* data_hdr, int64_t len) {
    if (!state || !data_hdr || len <= 0) return;
    XXH3_64bits_update((XXH3_state_t*) state,
                       ((const uint8_t*) data_hdr) + 8, (size_t) len);
}

int64_t __cajeta_xxh3_finish(void* state) {
    if (!state) return 0;
    return (int64_t) XXH3_64bits_digest((const XXH3_state_t*) state);
}

int64_t __cajeta_xxh3_oneshot(const void* data_hdr, int64_t len, int64_t seed) {
    if (!data_hdr || len <= 0) return 0;
    return (int64_t) XXH3_64bits_withSeed(
        ((const uint8_t*) data_hdr) + 8, (size_t) len, (uint64_t) seed);
}

// Width-named folders. Same approach as MD5 / SipHash.
void __cajeta_xxh3_write_i8(void* state, int8_t v) {
    if (state) XXH3_64bits_update((XXH3_state_t*) state, &v, 1);
}
void __cajeta_xxh3_write_i16(void* state, int16_t v) {
    if (!state) return;
    uint8_t b[2] = { (uint8_t) v, (uint8_t)(v >> 8) };
    XXH3_64bits_update((XXH3_state_t*) state, b, 2);
}
void __cajeta_xxh3_write_i32(void* state, int32_t v) {
    if (!state) return;
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (i * 8));
    XXH3_64bits_update((XXH3_state_t*) state, b, 4);
}
void __cajeta_xxh3_write_i64(void* state, int64_t v) {
    if (!state) return;
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (i * 8));
    XXH3_64bits_update((XXH3_state_t*) state, b, 8);
}
void __cajeta_xxh3_write_f32(void* state, float v) {
    if (!state) return;
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    __cajeta_xxh3_write_i32(state, (int32_t) bits);
}
void __cajeta_xxh3_write_f64(void* state, double v) {
    if (!state) return;
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    __cajeta_xxh3_write_i64(state, (int64_t) bits);
}

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

// Phase C — stat-touching helpers. POSIX-only.
//
// Each helper takes (bytes, length) — the Path's raw int8[] bytes
// and the byte count — since the cajeta-side `int8[]` storage
// doesn't carry a null terminator. The helpers copy into a stack
// buffer with `\0` appended and call the syscall. Returns int32
// (1/0 for predicates, -1 for hard errors that the cajeta-side
// will throw IoException for once the hierarchy is wired).

// Bound on stack-allocated path buffers. Linux PATH_MAX is 4096;
// callers with longer paths bail with the error sentinel.
#define __CAJETA_PATH_MAX 4096

// Copy `bytes[0..length)` into `dst` and append '\0'. Returns 0 on
// success, -1 if length >= dst_size (overflow guard).
static int __cajeta_copy_path_with_nul(
        char* dst, size_t dst_size,
        const char* bytes, int64_t length) {
    if (length < 0) length = 0;
    if ((size_t) length >= dst_size) return -1;
    if (length > 0 && bytes) memcpy(dst, bytes, (size_t) length);
    dst[length] = '\0';
    return 0;
}

int32_t __cajeta_path_exists(const char* bytes, int64_t length) {
    char path[__CAJETA_PATH_MAX];
    if (__cajeta_copy_path_with_nul(path, sizeof(path), bytes, length) != 0) return 0;
    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
}

int32_t __cajeta_path_is_file(const char* bytes, int64_t length) {
    char path[__CAJETA_PATH_MAX];
    if (__cajeta_copy_path_with_nul(path, sizeof(path), bytes, length) != 0) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

int32_t __cajeta_path_is_dir(const char* bytes, int64_t length) {
    char path[__CAJETA_PATH_MAX];
    if (__cajeta_copy_path_with_nul(path, sizeof(path), bytes, length) != 0) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int32_t __cajeta_path_is_symlink(const char* bytes, int64_t length) {
    char path[__CAJETA_PATH_MAX];
    if (__cajeta_copy_path_with_nul(path, sizeof(path), bytes, length) != 0) return 0;
    struct stat st;
    // lstat — symlink detection wants the link itself, not the target.
    if (lstat(path, &st) != 0) return 0;
    return S_ISLNK(st.st_mode) ? 1 : 0;
}

// Fill the caller-supplied FileInfo struct from a stat() call. The
// layout MUST match runtime/src/cajeta/io/file/FileInfo.cajeta:
//   { i64 size; i64 createdNanos; i64 modifiedNanos; i64 accessedNanos;
//     i1 isFile; i1 isDir; i1 isSymlink; i32 permissions; }
// Returns 0 on success, -1 if stat fails.
//
// Note: FileInfo's `bool` fields are i1 in the LLVM lowering and Cajeta
// stores them with a single byte. The vtable + the boolean fields'
// padding mean the bool stores are byte-wise aligned. We write each
// field via byte offsets computed at C compile time to avoid having to
// chase the exact LLVM struct layout from here. The compile-time
// offset arithmetic is wrapped in a struct mirror so the offsets line
// up with Cajeta's struct layout for FileInfo (8 + 8 + 8 + 8 + 1 + 1 +
// 1 + padding(1) + 4 = 40 bytes, with a leading 8-byte vtable slot
// = 48 total).
typedef struct {
    void*    vtable;
    int64_t  size;
    int64_t  createdNanos;
    int64_t  modifiedNanos;
    int64_t  accessedNanos;
    int8_t   isFile;
    int8_t   isDir;
    int8_t   isSymlink;
    int8_t   _pad;
    int32_t  permissions;
} __cajeta_FileInfoLayout;

int32_t __cajeta_path_stat(const char* path, void* fileInfo) {
    if (!path || !fileInfo) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    __cajeta_FileInfoLayout* fi = (__cajeta_FileInfoLayout*) fileInfo;
    fi->size = (int64_t) st.st_size;
    // POSIX-only ns granularity via st_*tim. On older systems fall back
    // to st_*time × 1e9.
#ifdef __linux__
    fi->createdNanos =
        (int64_t) st.st_ctim.tv_sec * 1000000000LL + (int64_t) st.st_ctim.tv_nsec;
    fi->modifiedNanos =
        (int64_t) st.st_mtim.tv_sec * 1000000000LL + (int64_t) st.st_mtim.tv_nsec;
    fi->accessedNanos =
        (int64_t) st.st_atim.tv_sec * 1000000000LL + (int64_t) st.st_atim.tv_nsec;
#else
    fi->createdNanos = (int64_t) st.st_ctime * 1000000000LL;
    fi->modifiedNanos = (int64_t) st.st_mtime * 1000000000LL;
    fi->accessedNanos = (int64_t) st.st_atime * 1000000000LL;
#endif
    fi->isFile = S_ISREG(st.st_mode) ? 1 : 0;
    fi->isDir = S_ISDIR(st.st_mode) ? 1 : 0;
    // L8: stat() follows symlinks, so st.st_mode is the TARGET's and S_ISLNK would
    // always be 0. lstat the path itself for the symlink flag (the other fields
    // intentionally describe the resolved target). On MinGW lstat==stat (stubbed),
    // preserving the existing "no symlinks" behavior there.
    struct stat lst;
    fi->isSymlink = (lstat(path, &lst) == 0 && S_ISLNK(lst.st_mode)) ? 1 : 0;
    fi->permissions = (int32_t) (st.st_mode & 07777);
    return 0;
}

// Phase D — directory + mutation helpers.

// Recursive mkdir. Mirrors `mkdir -p path` semantics: creates every
// missing intermediate component. Idempotent if the path already
// exists as a directory; fails (returns -1) if any intermediate
// component exists as a non-directory.
int32_t __cajeta_path_mkdirs(const char* bytes, int64_t length) {
    char path[__CAJETA_PATH_MAX];
    if (__cajeta_copy_path_with_nul(path, sizeof(path), bytes, length) != 0) return -1;
    if (path[0] == '\0') return -1;

    // Walk forward, inserting '\0' at each '/' boundary to mkdir
    // the intermediate prefix, then restoring the '/' before
    // continuing.
    for (char* p = path + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            struct stat st;
            if (stat(path, &st) != 0) {
                if (cajeta_mkdir(path, 0777) != 0 && errno != EEXIST) {
                    *p = '/';
                    return -1;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    // Final component.
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (cajeta_mkdir(path, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

// Single-file / empty-dir unlink. Returns 0 on success, -1 on
// failure (errno set; caller-side throw lands once IoException is
// wired). For non-empty directories, callers use
// `__cajeta_path_delete_recursive` (Phase D follow-up).
int32_t __cajeta_path_delete(const char* bytes, int64_t length) {
    char path[__CAJETA_PATH_MAX];
    if (__cajeta_copy_path_with_nul(path, sizeof(path), bytes, length) != 0) return -1;
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        return rmdir(path) == 0 ? 0 : -1;
    }
    return unlink(path) == 0 ? 0 : -1;
}

// realpath() wrapper. Returns a CajetaArray header containing the
// canonical absolute bytes, or NULL on failure. Path comes in as
// (bytes, length) — copy + null-terminate locally.
void* __cajeta_path_canonical(const char* bytes, int64_t length) {
    char in[__CAJETA_PATH_MAX];
    if (__cajeta_copy_path_with_nul(in, sizeof(in), bytes, length) != 0) return NULL;
    char* canon = realpath(in, NULL);
    if (!canon) return NULL;
    size_t n = strlen(canon);
    void* hdr = __cajeta_new_array_header(8, 1, (uint64_t) n);
    if (!hdr) {
        free(canon);
        return NULL;
    }
    memcpy(((char*) hdr) + 8, canon, n);
    free(canon);
    return hdr;
}

// ---------------------------------------------------------------------------
// At-exit registry — used by @PreDestroy synthesis (AspectModel.md § A11).
//
// The DI singleton @PreDestroy hook needs to fire at "process exit"
// semantics. libc's atexit() works for AOT binaries but dangles in
// JIT'd test runs: each test compiles a fresh module that's freed
// before the next test starts, so any function pointer registered
// from inside the JIT becomes invalid once that test's LLJIT state
// is destroyed. Routing through this runtime-internal registry lets
// the caller (a test, or main() in a real binary) explicitly fire
// handlers at a safe point and clear the list.
//
// Handlers run in LIFO order — the newest registration fires first.
// Each callback receives the instance pointer captured at register
// time; the user method must have signature `void (this:pointer)`
// (the ABI-compatible C type used here is `void (*)(void*)`).

typedef struct CajetaAtExitNode {
    void (*fn)(void*);
    void* arg;
    struct CajetaAtExitNode* next;
} CajetaAtExitNode;

static CajetaAtExitNode* __cajeta_atexit_head = NULL;

void __cajeta_atexit_push(void (*fn)(void*), void* arg) {
    if (!fn) return;
    CajetaAtExitNode* n = (CajetaAtExitNode*) malloc(sizeof(CajetaAtExitNode));
    if (!n) return;
    n->fn = fn;
    n->arg = arg;
    n->next = __cajeta_atexit_head;
    __cajeta_atexit_head = n;
}

void __cajeta_run_atexit_handlers(void) {
    CajetaAtExitNode* n = __cajeta_atexit_head;
    __cajeta_atexit_head = NULL;
    while (n) {
        CajetaAtExitNode* next = n->next;
        n->fn(n->arg);
        free(n);
        n = next;
    }
}

// ============================================================================
// cajeta.gpu.core runtime stubs (CajetaXPU phases 1-2, step 2).
//
// The cajeta.gpu.core stdlib classes (Stream / Event / Fence / Thread /
// Workgroup / Barrier / Wave) declare their methods @Native and forward to
// the symbols below. LLJIT eagerly materializes all externs at module load
// time, so these have to exist before any XPU implementation does.
//
// Every stub returns the zero value (NULL / 0 / false) or no-ops. Calling
// any of them in v1 yields a null Stream / zero coordinate / false flag —
// not a crash. Step 7 (CPU-emulation backend) replaces these with real
// implementations: thread-local globals for the coordinate readers, a host-
// side ordered queue for Stream/Event, etc. The native and Vulkan
// backends (steps 9-11) replace call sites at codegen time so these stubs
// only fire on the CPU-emulation path.
//
// Buffer<T>'s @Native methods are generic; they emit only when a Buffer<T>
// is instantiated, so no stubs appear here until a real allocator lands.
// ============================================================================

// ============================================================================
// CUDA Driver API binding (dlopen'd) — backs the real NVPTX device path.
// ============================================================================
// Mirrors src/cajeta/xpu/nvidia/CudaDriver.cpp, but lives in the runtime
// bitcode so the LLJIT host path resolves these symbols from merged bitcode
// (the C++ CudaDriver is not visible to the JIT'd module). The driver is
// bound lazily on first use; an absent GPU/driver leaves every entry a
// graceful no-op (alloc returns 0, copies/launch return silently) so host
// code that never touches the device still links and runs.

#if !defined(_WIN32)
#  include <dlfcn.h>
#endif

typedef unsigned long long cajeta_cudeviceptr;

struct cajeta_cuda_api {
    int loaded;            // 0 untried, 1 ready, -1 unavailable
    void* lib;
    void* ctx;
    int device;
    int (*cuInit)(unsigned);
    int (*cuDeviceGetCount)(int*);
    int (*cuDeviceGet)(int*, int);
    int (*cuCtxCreate)(void**, unsigned, int);
    int (*cuCtxSetCurrent)(void*);   // H9: bind the ctx to the launching thread
    int (*cuModuleLoadData)(void**, const void*);
    int (*cuModuleGetFunction)(void**, void*, const char*);
    int (*cuMemAlloc)(cajeta_cudeviceptr*, size_t);
    int (*cuMemcpyHtoD)(cajeta_cudeviceptr, const void*, size_t);
    int (*cuMemcpyDtoH)(void*, cajeta_cudeviceptr, size_t);
    int (*cuMemFree)(cajeta_cudeviceptr);
    // Pinned / unified (managed) memory (Buffer MemoryKind); optional — a missing
    // entry just falls that kind back to plain cuMemAlloc/cuMemFree. Managed
    // memory (cuMemAllocManaged) is one pointer host AND device see; pinned host
    // memory (cuMemHostAlloc) is page-locked + device-accessible, freed with
    // cuMemFreeHost (managed frees with plain cuMemFree).
    int (*cuMemAllocManaged)(cajeta_cudeviceptr*, size_t, unsigned);
    int (*cuMemHostAlloc)(void**, size_t, unsigned);
    int (*cuMemFreeHost)(void*);
    // Real streams + async copies; optional (bound non-fatally; null → default
    // stream + synchronous-memcpy fallback).
    int (*cuStreamCreate)(void**, unsigned);
    int (*cuStreamSynchronize)(void*);
    int (*cuStreamDestroy)(void*);
    int (*cuMemcpyHtoDAsync)(cajeta_cudeviceptr, const void*, size_t, void*);
    int (*cuMemcpyDtoHAsync)(void*, cajeta_cudeviceptr, size_t, void*);
    // Events (Event/Fence); optional. cuEventQuery returns CUDA_SUCCESS(0) when
    // complete, CUDA_ERROR_NOT_READY otherwise; cuStreamWaitEvent is the device-
    // side cross-stream wait.
    int (*cuEventCreate)(void**, unsigned);
    int (*cuEventRecord)(void*, void*);
    int (*cuEventSynchronize)(void*);
    int (*cuEventQuery)(void*);
    int (*cuStreamWaitEvent)(void*, void*, unsigned);
    int (*cuEventDestroy)(void*);
    int (*cuLaunchKernel)(void*, unsigned, unsigned, unsigned,
                          unsigned, unsigned, unsigned, unsigned,
                          void*, void**, void**);
    int (*cuCtxSynchronize)(void);
};
static struct cajeta_cuda_api g_xpu_cuda;                       // zero-initialized
static pthread_mutex_t g_xpu_cuda_lock = PTHREAD_MUTEX_INITIALIZER;

static void* cajeta_xpu_libsym(void* lib, const char* name) {
#if defined(_WIN32)
    return (void*) GetProcAddress((HMODULE) lib, name);
#else
    return dlsym(lib, name);
#endif
}

// Resolve the driver and create a context. Returns 1 on success. Caller holds
// g_xpu_cuda_lock. Idempotent via the `loaded` tri-state. The driver API
// exposes size-versioned symbols (cuMemAlloc_v2, …); we bind those explicitly.
static int cajeta_xpu_cuda_init_locked(void) {
    if (g_xpu_cuda.loaded == 1) return 1;
    if (g_xpu_cuda.loaded == -1) return 0;
    g_xpu_cuda.loaded = -1;  // assume failure until everything resolves
#if defined(_WIN32)
    g_xpu_cuda.lib = (void*) LoadLibraryA("nvcuda.dll");
#else
    g_xpu_cuda.lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
    if (!g_xpu_cuda.lib) return 0;
    #define CAJ_BIND(fp, nm)                                                  \
        do { *(void**)(&g_xpu_cuda.fp) = cajeta_xpu_libsym(g_xpu_cuda.lib, nm); \
             if (!g_xpu_cuda.fp) return 0; } while (0)
    CAJ_BIND(cuInit, "cuInit");
    CAJ_BIND(cuDeviceGetCount, "cuDeviceGetCount");
    CAJ_BIND(cuDeviceGet, "cuDeviceGet");
    CAJ_BIND(cuCtxCreate, "cuCtxCreate_v2");
    CAJ_BIND(cuCtxSetCurrent, "cuCtxSetCurrent");
    CAJ_BIND(cuModuleLoadData, "cuModuleLoadData");
    CAJ_BIND(cuModuleGetFunction, "cuModuleGetFunction");
    CAJ_BIND(cuMemAlloc, "cuMemAlloc_v2");
    CAJ_BIND(cuMemcpyHtoD, "cuMemcpyHtoD_v2");
    CAJ_BIND(cuMemcpyDtoH, "cuMemcpyDtoH_v2");
    CAJ_BIND(cuMemFree, "cuMemFree_v2");
    // Pinned / unified memory — optional (bound non-fatally; null → kind falls
    // back to plain device alloc/free).
    *(void**) (&g_xpu_cuda.cuMemAllocManaged) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuMemAllocManaged");
    *(void**) (&g_xpu_cuda.cuMemHostAlloc) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuMemHostAlloc");
    *(void**) (&g_xpu_cuda.cuMemFreeHost) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuMemFreeHost");
    // Real streams + async copies — optional (non-fatal).
    *(void**) (&g_xpu_cuda.cuStreamCreate) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuStreamCreate");
    *(void**) (&g_xpu_cuda.cuStreamSynchronize) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuStreamSynchronize");
    *(void**) (&g_xpu_cuda.cuStreamDestroy) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuStreamDestroy_v2");
    *(void**) (&g_xpu_cuda.cuMemcpyHtoDAsync) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuMemcpyHtoDAsync_v2");
    *(void**) (&g_xpu_cuda.cuMemcpyDtoHAsync) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuMemcpyDtoHAsync_v2");
    *(void**) (&g_xpu_cuda.cuEventCreate) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuEventCreate");
    *(void**) (&g_xpu_cuda.cuEventRecord) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuEventRecord");
    *(void**) (&g_xpu_cuda.cuEventSynchronize) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuEventSynchronize");
    *(void**) (&g_xpu_cuda.cuEventQuery) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuEventQuery");
    *(void**) (&g_xpu_cuda.cuStreamWaitEvent) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuStreamWaitEvent");
    *(void**) (&g_xpu_cuda.cuEventDestroy) =
        cajeta_xpu_libsym(g_xpu_cuda.lib, "cuEventDestroy_v2");
    CAJ_BIND(cuLaunchKernel, "cuLaunchKernel");
    CAJ_BIND(cuCtxSynchronize, "cuCtxSynchronize");
    #undef CAJ_BIND
    if (g_xpu_cuda.cuInit(0) != 0) return 0;
    int count = 0;
    if (g_xpu_cuda.cuDeviceGetCount(&count) != 0 || count <= 0) return 0;
    if (g_xpu_cuda.cuDeviceGet(&g_xpu_cuda.device, 0) != 0) return 0;
    if (g_xpu_cuda.cuCtxCreate(&g_xpu_cuda.ctx, 0, g_xpu_cuda.device) != 0) return 0;
    g_xpu_cuda.loaded = 1;
    return 1;
}

// Thread-safe "is the device usable?" gate. Once loaded == 1 the bound
// function pointers and single context are stable, so call sites read them
// unlocked after this returns true.
static int cajeta_xpu_cuda_ready(void) {
    int ok;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    ok = cajeta_xpu_cuda_init_locked();
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    return ok;
}

// ============================================================================
// HIP Driver API binding (dlopen'd) — backs the real AMDGPU device path.
// ============================================================================
// Mirrors src/cajeta/xpu/amd/HipDriver.cpp in C (the C++ HipDriver is
// compiler/test-only). HIP exports plain C symbols (no size-versioning). Shares
// g_xpu_cuda_lock for init/load serialization — only one device backend is
// active per run, so there is no contention. Device pointers are plain void*.
// --- HIP texture object ABI mirror (Item 8 Stage C) -------------------------
// The runtime resolves all HIP entry points by dlsym and never includes the
// ROCm headers (they're not on the default include path and carry C++), so the
// few structs hipCreateTextureObject needs are mirrored here with byte-exact
// layout. Enum values match driver_types.h / texture_types.h.
enum { CAJ_HIP_CHANNEL_SIGNED = 0 };    // hipChannelFormatKindSigned (R32I store)
enum { CAJ_HIP_CHANNEL_UNSIGNED = 1 };  // hipChannelFormatKindUnsigned (UNORM / R32UI store)
enum { CAJ_HIP_CHANNEL_FLOAT = 2 };     // hipChannelFormatKindFloat
enum { CAJ_HIP_RES_ARRAY = 0 };         // hipResourceTypeArray
enum { CAJ_HIP_RES_MIPMAPPED_ARRAY = 1 };  // hipResourceTypeMipmappedArray
enum { CAJ_HIP_ADDR_WRAP = 0, CAJ_HIP_ADDR_CLAMP = 1 };  // hipTextureAddressMode
enum { CAJ_HIP_FILTER_POINT = 0, CAJ_HIP_FILTER_LINEAR = 1 };  // filter mode
enum { CAJ_HIP_READ_ELEMENT = 0 };      // hipReadModeElementType
enum { CAJ_HIP_READ_NORMALIZED_FLOAT = 1 };  // hipReadModeNormalizedFloat (UNORM→[0,1])
enum { CAJ_HIP_MEMCPY_HTOD = 1 };       // hipMemcpyHostToDevice
enum { CAJ_HIP_MEMCPY_DTOH = 2 };       // hipMemcpyDeviceToHost
// hipArray creation flags (driver_types.h; mirror the CUDA values).
enum { CAJ_HIP_ARRAY_LAYERED = 0x01 };  // hipArrayLayered (2-D array)
enum { CAJ_HIP_ARRAY_SURFACE_LOAD_STORE = 0x02 };  // hipArraySurfaceLoadStore (Image2D)
enum { CAJ_HIP_ARRAY_CUBEMAP = 0x04 };  // hipArrayCubemap (6-face cube)

struct caj_hip_channel_format_desc { int x, y, z, w; int f; };
struct caj_hip_resource_desc {
    int resType;
    union {
        struct { void* array; } array;
        struct { void* mipmap; } mipmap;
        struct { void* devPtr; struct caj_hip_channel_format_desc desc;
                 size_t sizeInBytes; } linear;
        struct { void* devPtr; struct caj_hip_channel_format_desc desc;
                 size_t width, height, pitchInBytes; } pitch2D;
    } res;
};
struct caj_hip_texture_desc {
    int addressMode[3];
    int filterMode;
    int readMode;
    int sRGB;
    float borderColor[4];
    int normalizedCoords;
    unsigned int maxAnisotropy;
    int mipmapFilterMode;
    float mipmapLevelBias;
    float minMipmapLevelClamp;
    float maxMipmapLevelClamp;
};
// 3-D array ABI mirrors (Texture3D). Byte-exact with HIP driver_types.h.
struct caj_hip_extent { size_t w, h, d; };          // hipExtent {width,height,depth}
struct caj_hip_pos { size_t x, y, z; };             // hipPos
struct caj_hip_pitched_ptr {                        // hipPitchedPtr
    void* ptr; size_t pitch; size_t xsize; size_t ysize;
};
struct caj_hip_memcpy3d_parms {                     // hipMemcpy3DParms
    void* srcArray;
    struct caj_hip_pos srcPos;
    struct caj_hip_pitched_ptr srcPtr;
    void* dstArray;
    struct caj_hip_pos dstPos;
    struct caj_hip_pitched_ptr dstPtr;
    struct caj_hip_extent extent;
    int kind;
};

struct cajeta_hip_api {
    int loaded;             // 0 untried, 1 ready, -1 unavailable
    void* lib;
    int device;
    int (*hipInit)(unsigned);
    int (*hipGetDeviceCount)(int*);
    int (*hipSetDevice)(int);
    int (*hipModuleLoadData)(void**, const void*);
    int (*hipModuleGetFunction)(void**, void*, const char*);
    int (*hipMalloc)(void**, size_t);
    int (*hipMemcpyHtoD)(void*, const void*, size_t);
    int (*hipMemcpyDtoH)(void*, void*, size_t);
    int (*hipFree)(void*);
    int (*hipModuleLaunchKernel)(void*, unsigned, unsigned, unsigned,
                                 unsigned, unsigned, unsigned, unsigned,
                                 void*, void**, void**);
    int (*hipDeviceSynchronize)(void);
    // Texture object path (Item 8 Stage C); optional — absent on very old HIP,
    // in which case texture sampling on AMD is simply unavailable.
    int (*hipMallocArray)(void**, const void*, size_t, size_t, unsigned);
    int (*hipFreeArray)(void*);
    int (*hipMemcpy2DToArray)(void*, size_t, size_t, const void*, size_t,
                              size_t, size_t, int);
    int (*hipCreateTextureObject)(void**, const void*, const void*,
                                  const void*);
    int (*hipDestroyTextureObject)(void*);
    // Surface object path (Image2D storage images, the writable twin of the
    // texture object); optional — absent → AMD storage images unavailable (the
    // path degrades like mipmaps). hipCreateSurfaceObject builds a writable
    // surface from an ARRAY resource desc (no sampler); the array must be
    // allocated with hipArraySurfaceLoadStore. hipMemcpy2DFromArray reads it back.
    int (*hipCreateSurfaceObject)(void**, const void*);
    int (*hipDestroySurfaceObject)(void*);
    int (*hipMemcpy2DFromArray)(void*, size_t, const void*, size_t, size_t,
                                size_t, size_t, int);
    // 3-D array path (Texture3D); optional — absent → 3-D textures unavailable on AMD.
    int (*hipMalloc3DArray)(void**, const void*, struct caj_hip_extent, unsigned);
    int (*hipMemcpy3D)(const void*);
    // Mipmapped-array path (mip Texture2D); optional — absent → mip textures
    // unavailable on AMD. hipMallocMipmappedArray takes the extent by value
    // ({w, h, 0} for 2-D), the level count, and flags; hipGetMipmappedArrayLevel
    // yields a plain hipArray for one level (copied into via hipMemcpy2DToArray).
    int (*hipMallocMipmappedArray)(void**, const void*, struct caj_hip_extent,
                                   unsigned, unsigned);
    int (*hipGetMipmappedArrayLevel)(void**, void*, unsigned);
    int (*hipFreeMipmappedArray)(void*);
    // Pinned / unified (managed) memory (Buffer MemoryKind); optional — absent →
    // those kinds fall back to plain hipMalloc. hipMallocManaged gives one
    // pointer accessible from host AND device (zero-copy on an APU like Strix
    // Halo); hipHostMalloc gives page-locked, device-accessible host memory
    // (fast/async DMA); hipHostFree releases the latter (managed memory frees
    // with plain hipFree).
    int (*hipMallocManaged)(void**, size_t, unsigned);
    int (*hipHostMalloc)(void**, size_t, unsigned);
    int (*hipHostFree)(void*);
    // Real streams + async copies (Buffer.uploadAsync/downloadAsync, stream-
    // ordered launch); optional — absent → stream create no-ops to the default
    // stream and async copies fall back to the synchronous memcpy.
    int (*hipStreamCreate)(void**);
    int (*hipStreamSynchronize)(void*);
    int (*hipStreamDestroy)(void*);
    int (*hipMemcpyHtoDAsync)(void*, const void*, size_t, void*);
    int (*hipMemcpyDtoHAsync)(void*, void*, size_t, void*);
    // Events (Event/Fence cross-stream + host sync); optional — absent → the
    // synchronous fallback (record/wait no-op, query true). hipEventQuery returns
    // hipSuccess(0) when complete, hipErrorNotReady otherwise; hipStreamWaitEvent
    // makes a stream wait on another stream's recorded event (device-side).
    int (*hipEventCreate)(void**);
    int (*hipEventRecord)(void*, void*);
    int (*hipEventSynchronize)(void*);
    int (*hipEventQuery)(void*);
    int (*hipStreamWaitEvent)(void*, void*, unsigned);
    int (*hipEventDestroy)(void*);
};
static struct cajeta_hip_api g_xpu_hip;

// HIP texture record: a Texture2D's device handle on AMD is a 1-based-... no, a
// pointer to one of these (the hipArray + dims). The hipTextureObject is built
// per launch from this array + the paired Sampler's modes.
// `array` is the (level-0) plain hipArray for non-mip 2-D/3-D; `mipmap` is the
// hipMipmappedArray for a mip Texture2D (NULL otherwise) and `levels` its level
// count (1 = no mipmaps). A mip texture keeps `array` NULL — its per-level arrays
// come from hipGetMipmappedArrayLevel; the texobj binds the mipmapped array.
struct cajeta_hip_tex {
    void* array; void* mipmap; uint32_t w, h, d; int32_t format; int levels;
};

// --- Texture format table ----------------------------------------------------
// TextureFormat ordinals — MUST match runtime/src/cajeta/xpu/core/TextureFormat.cajeta.
// All four are float-sampled (sample() returns a vec4): float formats read back
// as-is, UNORM formats store a byte 0..255 and read back normalized to [0,1].
#define CAJ_TEXFMT_R32F        0
#define CAJ_TEXFMT_R8_UNORM    1
#define CAJ_TEXFMT_RGBA8_UNORM 2
#define CAJ_TEXFMT_RGBA32F     3
#define CAJ_TEXFMT_R16F        4
#define CAJ_TEXFMT_RGBA16F     5
#define CAJ_TEXFMT_R32I        6   // 1ch 32-bit signed int   — fetch-only (raw, no convert)
#define CAJ_TEXFMT_R32UI       7   // 1ch 32-bit unsigned int — fetch-only
#define CAJ_TEXFMT_RGBA32I     8   // 4ch 32-bit signed int   — fetch-only
#define CAJ_TEXFMT_RGBA32UI    9   // 4ch 32-bit unsigned int — fetch-only

static inline int cajeta_texfmt_channels(int32_t fmt) {
    return (fmt == CAJ_TEXFMT_RGBA8_UNORM || fmt == CAJ_TEXFMT_RGBA32F ||
            fmt == CAJ_TEXFMT_RGBA16F     || fmt == CAJ_TEXFMT_RGBA32I ||
            fmt == CAJ_TEXFMT_RGBA32UI) ? 4 : 1;
}
static inline int cajeta_texfmt_is_unorm(int32_t fmt) {
    return fmt == CAJ_TEXFMT_R8_UNORM || fmt == CAJ_TEXFMT_RGBA8_UNORM;
}
// Half-float (16-bit IEEE binary16) storage formats — the cheap-HDR path.
static inline int cajeta_texfmt_is_half(int32_t fmt) {
    return fmt == CAJ_TEXFMT_R16F || fmt == CAJ_TEXFMT_RGBA16F;
}
// Raw 32-bit integer storage formats (signed or unsigned) — stored and fetched
// verbatim (no normalization / float convert); fetch-only (no hardware filter).
static inline int cajeta_texfmt_is_integer(int32_t fmt) {
    return fmt == CAJ_TEXFMT_R32I  || fmt == CAJ_TEXFMT_R32UI ||
           fmt == CAJ_TEXFMT_RGBA32I || fmt == CAJ_TEXFMT_RGBA32UI;
}
// True for the unsigned integer formats (HIP channel-kind / VK *_UINT select).
static inline int cajeta_texfmt_is_unsigned(int32_t fmt) {
    return fmt == CAJ_TEXFMT_R32UI || fmt == CAJ_TEXFMT_RGBA32UI;
}
// Bytes per channel in device storage (UNORM = 1, half = 2, float/int = 4).
static inline size_t cajeta_texfmt_channel_bytes(int32_t fmt) {
    if (cajeta_texfmt_is_unorm(fmt)) return 1u;
    if (cajeta_texfmt_is_half(fmt))  return 2u;
    return 4u;
}
// Bytes per texel in device storage.
static inline size_t cajeta_texfmt_texel_bytes(int32_t fmt) {
    return (size_t) cajeta_texfmt_channels(fmt) * cajeta_texfmt_channel_bytes(fmt);
}
// Quantize one float [0,1] to a UNORM byte (round-to-nearest, clamped).
static inline unsigned char cajeta_texfmt_unorm8(float f) {
    if (f <= 0.0f) return 0;
    if (f >= 1.0f) return 255;
    return (unsigned char) (f * 255.0f + 0.5f);
}
// Convert one float32 to IEEE 754 binary16 (round-to-nearest-even), returned as
// the raw 16-bit pattern. Handles sign, subnormals, overflow→Inf, and NaN.
static inline uint16_t cajeta_f32_to_f16(float f) {
    uint32_t x;
    memcpy(&x, &f, sizeof(x));
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t  exp  = (int32_t) ((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFFu) == 0xFFu) {                 // Inf / NaN
        return (uint16_t) (sign | 0x7C00u | (mant ? 0x200u : 0u));
    }
    if (exp >= 0x1F) return (uint16_t) (sign | 0x7C00u);  // overflow → Inf
    if (exp <= 0) {                                      // subnormal / zero
        if (exp < -10) return (uint16_t) sign;
        mant |= 0x800000u;
        uint32_t shift = (uint32_t) (14 - exp);
        uint32_t half  = mant >> shift;
        uint32_t rem   = mant & ((1u << shift) - 1u);
        uint32_t mid   = 1u << (shift - 1);
        if (rem > mid || (rem == mid && (half & 1u))) half++;
        return (uint16_t) (sign | half);
    }
    uint16_t half = (uint16_t) (sign | ((uint32_t) exp << 10) | (mant >> 13));
    uint32_t rem = mant & 0x1FFFu;                       // round-to-nearest-even
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) half++;
    return half;
}
// Convert one IEEE 754 binary16 bit pattern back to float32 (exact).
static inline float cajeta_f16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t) (h & 0x8000u) << 16;
    uint32_t exp  = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) { bits = sign; }                 // +/- zero
        else {                                          // subnormal
            exp = 127 - 15 + 1;
            while ((mant & 0x400u) == 0) { mant <<= 1; exp--; }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1Fu) {                           // Inf / NaN
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}
// Encode `texels` (= w*h*channels) channel-interleaved floats from `src` into
// `dst` in the storage format: float → memcpy, half → binary16, UNORM →
// quantized bytes. `dst` must hold texels * channel_bytes.
static void cajeta_texfmt_encode(void* dst, const float* src, size_t texels,
                                 int32_t fmt) {
    if (cajeta_texfmt_is_unorm(fmt)) {
        unsigned char* b = (unsigned char*) dst;
        for (size_t i = 0; i < texels; ++i) b[i] = cajeta_texfmt_unorm8(src[i]);
    } else if (cajeta_texfmt_is_half(fmt)) {
        uint16_t* h = (uint16_t*) dst;
        for (size_t i = 0; i < texels; ++i) h[i] = cajeta_f32_to_f16(src[i]);
    } else {
        memcpy(dst, src, texels * sizeof(float));
    }
}

#if !defined(_WIN32)
// Load libamdhip64, preferring canonical ROCm (/opt/rocm, the
// update-alternatives target) then $ROCM_PATH over a bare soname; pin the
// chosen dir's libhsa-runtime with RTLD_GLOBAL first so hip's transitive HSA
// dependency binds to it by soname (see HipDriver.cpp for the rationale).
static void* cajeta_xpu_load_hip_from_dir(const char* dir) {
    char hsa[600], hip[600];
    snprintf(hsa, sizeof(hsa), "%s/libhsa-runtime64.so.1", dir);
    snprintf(hip, sizeof(hip), "%s/libamdhip64.so", dir);
    dlopen(hsa, RTLD_NOW | RTLD_GLOBAL);          // pin canonical HSA (best-effort)
    return dlopen(hip, RTLD_NOW | RTLD_LOCAL);
}
static void* cajeta_xpu_load_hip(void) {
    void* h = cajeta_xpu_load_hip_from_dir("/opt/rocm/lib");
    if (h) return h;
    const char* rp = getenv("ROCM_PATH");
    if (rp) {
        char dir[520];
        snprintf(dir, sizeof(dir), "%s/lib", rp);
        if ((h = cajeta_xpu_load_hip_from_dir(dir))) return h;
    }
    if ((h = dlopen("libamdhip64.so", RTLD_NOW | RTLD_LOCAL))) return h;
    return dlopen("libamdhip64.so.7", RTLD_NOW | RTLD_LOCAL);
}
#endif

// Caller holds g_xpu_cuda_lock. Idempotent via the `loaded` tri-state.
static int cajeta_xpu_hip_init_locked(void) {
    if (g_xpu_hip.loaded == 1) return 1;
    if (g_xpu_hip.loaded == -1) return 0;
    g_xpu_hip.loaded = -1;
#if defined(_WIN32)
    g_xpu_hip.lib = (void*) LoadLibraryA("amdhip64.dll");
#else
    g_xpu_hip.lib = cajeta_xpu_load_hip();
#endif
    if (!g_xpu_hip.lib) return 0;
    #define CAJ_HBIND(fp, nm)                                                  \
        do { *(void**)(&g_xpu_hip.fp) = cajeta_xpu_libsym(g_xpu_hip.lib, nm);  \
             if (!g_xpu_hip.fp) return 0; } while (0)
    CAJ_HBIND(hipInit, "hipInit");
    CAJ_HBIND(hipGetDeviceCount, "hipGetDeviceCount");
    CAJ_HBIND(hipSetDevice, "hipSetDevice");
    CAJ_HBIND(hipModuleLoadData, "hipModuleLoadData");
    CAJ_HBIND(hipModuleGetFunction, "hipModuleGetFunction");
    CAJ_HBIND(hipMalloc, "hipMalloc");
    CAJ_HBIND(hipMemcpyHtoD, "hipMemcpyHtoD");
    CAJ_HBIND(hipMemcpyDtoH, "hipMemcpyDtoH");
    CAJ_HBIND(hipFree, "hipFree");
    CAJ_HBIND(hipModuleLaunchKernel, "hipModuleLaunchKernel");
    CAJ_HBIND(hipDeviceSynchronize, "hipDeviceSynchronize");
    #undef CAJ_HBIND
    // Texture object path (Item 8 Stage C) — optional; a missing entry just
    // disables AMD texture sampling, it doesn't fail the whole HIP backend.
    #define CAJ_HBIND_OPT(fp, nm)                                              \
        *(void**) (&g_xpu_hip.fp) = cajeta_xpu_libsym(g_xpu_hip.lib, nm)
    CAJ_HBIND_OPT(hipMallocArray, "hipMallocArray");
    CAJ_HBIND_OPT(hipFreeArray, "hipFreeArray");
    CAJ_HBIND_OPT(hipMemcpy2DToArray, "hipMemcpy2DToArray");
    CAJ_HBIND_OPT(hipCreateTextureObject, "hipCreateTextureObject");
    CAJ_HBIND_OPT(hipDestroyTextureObject, "hipDestroyTextureObject");
    CAJ_HBIND_OPT(hipCreateSurfaceObject, "hipCreateSurfaceObject");
    CAJ_HBIND_OPT(hipDestroySurfaceObject, "hipDestroySurfaceObject");
    CAJ_HBIND_OPT(hipMemcpy2DFromArray, "hipMemcpy2DFromArray");
    CAJ_HBIND_OPT(hipMalloc3DArray, "hipMalloc3DArray");
    CAJ_HBIND_OPT(hipMemcpy3D, "hipMemcpy3D");
    CAJ_HBIND_OPT(hipMallocMipmappedArray, "hipMallocMipmappedArray");
    CAJ_HBIND_OPT(hipGetMipmappedArrayLevel, "hipGetMipmappedArrayLevel");
    CAJ_HBIND_OPT(hipFreeMipmappedArray, "hipFreeMipmappedArray");
    CAJ_HBIND_OPT(hipMallocManaged, "hipMallocManaged");
    CAJ_HBIND_OPT(hipHostMalloc, "hipHostMalloc");
    CAJ_HBIND_OPT(hipHostFree, "hipHostFree");
    CAJ_HBIND_OPT(hipStreamCreate, "hipStreamCreate");
    CAJ_HBIND_OPT(hipStreamSynchronize, "hipStreamSynchronize");
    CAJ_HBIND_OPT(hipStreamDestroy, "hipStreamDestroy");
    CAJ_HBIND_OPT(hipMemcpyHtoDAsync, "hipMemcpyHtoDAsync");
    CAJ_HBIND_OPT(hipMemcpyDtoHAsync, "hipMemcpyDtoHAsync");
    CAJ_HBIND_OPT(hipEventCreate, "hipEventCreate");
    CAJ_HBIND_OPT(hipEventRecord, "hipEventRecord");
    CAJ_HBIND_OPT(hipEventSynchronize, "hipEventSynchronize");
    CAJ_HBIND_OPT(hipEventQuery, "hipEventQuery");
    CAJ_HBIND_OPT(hipStreamWaitEvent, "hipStreamWaitEvent");
    CAJ_HBIND_OPT(hipEventDestroy, "hipEventDestroy");
    #undef CAJ_HBIND_OPT
    if (g_xpu_hip.hipInit(0) != 0) return 0;
    int count = 0;
    if (g_xpu_hip.hipGetDeviceCount(&count) != 0 || count <= 0) return 0;
    if (g_xpu_hip.hipSetDevice(g_xpu_hip.device) != 0) return 0;
    g_xpu_hip.loaded = 1;
    return 1;
}

// --- per-kernel parameter metadata (the Vulkan launch translation) ----------
// The compiler registers, per Vulkan-bundled @Kernel, which args are buffers vs
// scalars and the scalar byte sizes. The Vulkan launch path uses it to turn the
// uniform kernelParams argv into descriptor bindings: buffers map to existing
// storage buffers; scalars are copied into transient single-element SSBOs. The
// pointers are program constant data (valid for the process lifetime).
// Per-param kind in the launch metadata (matches xpu::KernelParamInfo::kind):
// 0 = scalar (by-value primitive/POD → single-element SSBO), 1 = buffer,
// 2 = texture (Texture2D → sampled image), 3 = sampler (Item 8 Stage B).
#define CAJETA_KP_SCALAR  0
#define CAJETA_KP_BUFFER  1
#define CAJETA_KP_TEXTURE 2
#define CAJETA_KP_SAMPLER 3
#define CAJETA_KP_ACCEL   4   // AccelerationStructure -> descriptor-bound BVH
#define CAJETA_KP_IMAGE   5   // Image2D (writable) -> STORAGE_IMAGE descriptor
#define CAJETA_KP_BUFFER_ARRAY 6   // Buffer<T>[] -> bindless descriptor array
                                   // (descriptorCount = N; N + handles in argv slot)

struct cajeta_kparams {
    char name[256];
    int count;
    const uint8_t* kind;
    const uint32_t* byteSize;
};
#define CAJETA_XPU_MAX_KPARAMS 128
static struct cajeta_kparams g_xpu_kparams[CAJETA_XPU_MAX_KPARAMS];
static int g_xpu_kparam_count;

void __cajeta_xpu_register_kernel_params(const char* name, int32_t count,
                                         const uint8_t* kind,
                                         const uint32_t* byteSize) {
    if (!name) return;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    // M3: dedup by name — re-registration (e.g. a re-run or a second backend)
    // overwrites the existing entry instead of appending a stale duplicate and
    // exhausting the fixed table (M backends would otherwise fill it at
    // CAJETA_XPU_MAX_KPARAMS/M kernels).
    int idx = -1;
    for (int i = 0; i < g_xpu_kparam_count; ++i)
        if (strncmp(g_xpu_kparams[i].name, name,
                    sizeof(g_xpu_kparams[i].name)) == 0) { idx = i; break; }
    int isNew = 0;
    if (idx < 0) {
        if (g_xpu_kparam_count >= CAJETA_XPU_MAX_KPARAMS) {
            pthread_mutex_unlock(&g_xpu_cuda_lock);
            return;
        }
        idx = g_xpu_kparam_count;
        isNew = 1;
    }
    struct cajeta_kparams* e = &g_xpu_kparams[idx];
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    e->count = count;
    e->kind = kind;
    e->byteSize = byteSize;
    // L9: publish a new slot only after its fields are fully written, so a
    // lock-free find_kparams can't observe an entry with a stale field set.
    if (isNew) g_xpu_kparam_count++;
    pthread_mutex_unlock(&g_xpu_cuda_lock);
}

static struct cajeta_kparams* cajeta_xpu_find_kparams(const char* name) {
    for (int i = 0; i < g_xpu_kparam_count; ++i)
        if (strncmp(g_xpu_kparams[i].name, name,
                    sizeof(g_xpu_kparams[i].name)) == 0)
            return &g_xpu_kparams[i];
    return NULL;
}

// ============================================================================
// Vulkan compute binding (dlopen'd) — backs the real SPIR-V device path.
// ============================================================================
// Mirrors src/cajeta/xpu/vulkan/VulkanDriver.cpp in C. Compiled in only when a
// Vulkan SDK header is present at runtime-build time; otherwise the entry points
// are no-ops and Vulkan probes unavailable. All Vulkan functions are resolved at
// runtime via vkGetInstanceProcAddr/vkGetDeviceProcAddr (no link dependency).
#if defined(__has_include)
#  if __has_include(<vulkan/vulkan.h>)
#    define CAJETA_RT_HAS_VULKAN 1
#  endif
#endif

#if defined(CAJETA_RT_HAS_VULKAN) && !defined(_WIN32)
#include <vulkan/vulkan.h>

struct cajeta_vk {
    int loaded;                  // 0 untried, 1 ready, -1 unavailable
    void* lib;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t queueFamily;
    VkCommandPool cmdPool;
    VkPhysicalDeviceMemoryProperties memProps;
    PFN_vkCreateInstance vkCreateInstance;
    PFN_vkDestroyInstance vkDestroyInstance;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
    PFN_vkCreateDevice vkCreateDevice;
    PFN_vkDestroyDevice vkDestroyDevice;
    PFN_vkGetDeviceQueue vkGetDeviceQueue;
    PFN_vkCreateBuffer vkCreateBuffer;
    PFN_vkDestroyBuffer vkDestroyBuffer;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
    PFN_vkAllocateMemory vkAllocateMemory;
    PFN_vkFreeMemory vkFreeMemory;
    PFN_vkBindBufferMemory vkBindBufferMemory;
    PFN_vkMapMemory vkMapMemory;
    PFN_vkUnmapMemory vkUnmapMemory;
    // Image + sampler path (Item 8 Stage B: Texture2D / Sampler).
    PFN_vkCreateImage vkCreateImage;
    PFN_vkDestroyImage vkDestroyImage;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
    PFN_vkBindImageMemory vkBindImageMemory;
    PFN_vkCreateImageView vkCreateImageView;
    PFN_vkDestroyImageView vkDestroyImageView;
    PFN_vkCreateSampler vkCreateSampler;
    PFN_vkDestroySampler vkDestroySampler;
    PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
    PFN_vkCreateShaderModule vkCreateShaderModule;
    PFN_vkDestroyShaderModule vkDestroyShaderModule;
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
    PFN_vkCreateComputePipelines vkCreateComputePipelines;
    PFN_vkDestroyPipeline vkDestroyPipeline;
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;
    PFN_vkCreateCommandPool vkCreateCommandPool;
    PFN_vkDestroyCommandPool vkDestroyCommandPool;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer vkEndCommandBuffer;
    PFN_vkCmdBindPipeline vkCmdBindPipeline;
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
    PFN_vkCmdDispatch vkCmdDispatch;
    PFN_vkQueueSubmit vkQueueSubmit;
    PFN_vkQueueWaitIdle vkQueueWaitIdle;
    // Ray-query / acceleration-structure path (cajeta-gpu Part C inc 3b).
    // Resolved + the device extensions/features enabled only when the physical
    // device supports VK_KHR_acceleration_structure + VK_KHR_ray_query +
    // buffer-device-address; `rayQuery` stays 0 otherwise (and the AS natives
    // no-op) so the compute buffer/texture path is unaffected on non-RT GPUs.
    int rayQuery;                // 1 if AS/ray-query is usable on this device
    PFN_vkGetBufferDeviceAddress vkGetBufferDeviceAddress;
    PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR;
    PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR;
    PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR;
    PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR;
    PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR;
    // minAccelerationStructureScratchOffsetAlignment — the BVH build scratch
    // device address must be rounded up to this (VUID-...-scratchData-03710).
    VkDeviceSize scratchAlign;
};
static struct cajeta_vk g_xpu_vk;

// Serializes all VkQueue submits + VkCommandPool use AND every resource-table
// mutation/read (the buffer g_vk_bufs, texture g_vk_texs, and AS g_vk_accels
// tables). A VkQueue and a VkCommandPool require external host synchronization,
// and the tables are plain arrays + counts; the launch/build/free/alloc paths can
// be driven from different OS threads (the program's main thread vs a carrier-
// fiber thread), so without this they race the shared queue/pool/tables.
// RECURSIVE: the launch path holds this across the whole dispatch and calls the
// table accessors (cajeta_xpu_vk_rec / _tex_rec) under it, so the accessors must
// be able to re-lock. Distinct from g_xpu_cuda_lock (backend init/load only).
// Initialized at runtime in cajeta_xpu_vulkan_init_locked (the portable static
// recursive initializer needs _GNU_SOURCE, which this TU doesn't set); the glibc
// recursive enum is the _NP spelling, macOS/Windows use the unsuffixed one.
#if defined(__APPLE__) || defined(_WIN32)
#  define CAJETA_MUTEX_RECURSIVE PTHREAD_MUTEX_RECURSIVE
#else
#  define CAJETA_MUTEX_RECURSIVE PTHREAD_MUTEX_RECURSIVE_NP
#endif
static pthread_mutex_t g_xpu_vk_submit_mu;

struct cajeta_vk_buf {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapped;
    VkDeviceSize size;
    int live;
    // Sub-buffer view (Buffer.slice): a view slot borrows a parent's buffer/
    // memory (does NOT own them — free() must not destroy them) and carries the
    // byte offset bound into the descriptor (VkDescriptorBufferInfo.offset) and
    // folded into `mapped` for host upload/download. is_view==0 for an owner.
    int is_view;
    VkDeviceSize view_offset;
};
#define CAJETA_VK_MAX_BUFFERS 4096
static struct cajeta_vk_buf g_vk_bufs[CAJETA_VK_MAX_BUFFERS];
static int g_vk_buf_count;

static int cajeta_xpu_vk_find_memory_type(uint32_t typeBits,
                                          VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_xpu_vk.memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (g_xpu_vk.memProps.memoryTypes[i].propertyFlags & want) == want)
            return (int) i;
    }
    return -1;
}

// Caller holds g_xpu_cuda_lock. Bring up instance/device/queue/cmdpool.
static int cajeta_xpu_vulkan_init_locked(void) {
    if (g_xpu_vk.loaded == 1) return 1;
    if (g_xpu_vk.loaded == -1) return 0;
    g_xpu_vk.loaded = -1;

    // One-time init of the recursive submit/table mutex (this runs exactly once —
    // the tri-state above gates it — and before any buffer/texture/launch use,
    // all of which go through this init first via cajeta_xpu_active_backend).
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, CAJETA_MUTEX_RECURSIVE);
        pthread_mutex_init(&g_xpu_vk_submit_mu, &attr);
        pthread_mutexattr_destroy(&attr);
    }

#if defined(__APPLE__)
    // MV1: macOS has no native Vulkan ICD — load MoltenVK (Vulkan->Metal). The
    // LunarG SDK installs libvulkan.1.dylib; a bare MoltenVK install ships
    // libMoltenVK.dylib. (On-device validation is gated on Apple hardware.)
    const char* libnames[] = {"libvulkan.1.dylib", "libvulkan.dylib",
                              "libMoltenVK.dylib"};
#else
    const char* libnames[] = {"libvulkan.so.1", "libvulkan.so"};
#endif
    const int nlibs = (int) (sizeof(libnames) / sizeof(libnames[0]));
    for (int i = 0; i < nlibs && !g_xpu_vk.lib; ++i)
        g_xpu_vk.lib = dlopen(libnames[i], RTLD_NOW | RTLD_LOCAL);
    if (!g_xpu_vk.lib) return 0;
    g_xpu_vk.getInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr) dlsym(g_xpu_vk.lib, "vkGetInstanceProcAddr");
    if (!g_xpu_vk.getInstanceProcAddr) return 0;

    g_xpu_vk.vkCreateInstance = (PFN_vkCreateInstance)
        g_xpu_vk.getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!g_xpu_vk.vkCreateInstance) return 0;

    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "cajeta-xpu";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (g_xpu_vk.vkCreateInstance(&ici, NULL, &g_xpu_vk.instance) != VK_SUCCESS)
        return 0;

    #define CAJ_VKI(nm) g_xpu_vk.nm = (PFN_##nm)                               \
        g_xpu_vk.getInstanceProcAddr(g_xpu_vk.instance, #nm)
    CAJ_VKI(vkDestroyInstance);
    CAJ_VKI(vkEnumeratePhysicalDevices);
    CAJ_VKI(vkGetPhysicalDeviceQueueFamilyProperties);
    CAJ_VKI(vkGetPhysicalDeviceMemoryProperties);
    CAJ_VKI(vkCreateDevice);
    CAJ_VKI(vkDestroyDevice);
    // Optional (ray-query detection): present on any 1.1+ ICD. Resolved here so
    // bring-up can probe AS/ray-query support before vkCreateDevice.
    PFN_vkEnumerateDeviceExtensionProperties enumDevExt =
        (PFN_vkEnumerateDeviceExtensionProperties) g_xpu_vk.getInstanceProcAddr(
            g_xpu_vk.instance, "vkEnumerateDeviceExtensionProperties");
    PFN_vkGetPhysicalDeviceFeatures2 getFeatures2 =
        (PFN_vkGetPhysicalDeviceFeatures2) g_xpu_vk.getInstanceProcAddr(
            g_xpu_vk.instance, "vkGetPhysicalDeviceFeatures2");
    PFN_vkGetPhysicalDeviceProperties2 getProps2 =
        (PFN_vkGetPhysicalDeviceProperties2) g_xpu_vk.getInstanceProcAddr(
            g_xpu_vk.instance, "vkGetPhysicalDeviceProperties2");
    #undef CAJ_VKI
    g_xpu_vk.getDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
        g_xpu_vk.getInstanceProcAddr(g_xpu_vk.instance, "vkGetDeviceProcAddr");
    if (!g_xpu_vk.vkEnumeratePhysicalDevices || !g_xpu_vk.vkCreateDevice ||
        !g_xpu_vk.getDeviceProcAddr)
        return 0;

    uint32_t count = 0;
    g_xpu_vk.vkEnumeratePhysicalDevices(g_xpu_vk.instance, &count, NULL);
    if (count == 0) return 0;
    if (count > 16) count = 16;
    VkPhysicalDevice devs[16];
    g_xpu_vk.vkEnumeratePhysicalDevices(g_xpu_vk.instance, &count, devs);

    int found = 0;
    for (uint32_t di = 0; di < count && !found; ++di) {
        uint32_t qn = 0;
        g_xpu_vk.vkGetPhysicalDeviceQueueFamilyProperties(devs[di], &qn, NULL);
        if (qn > 32) qn = 32;
        VkQueueFamilyProperties qp[32];
        g_xpu_vk.vkGetPhysicalDeviceQueueFamilyProperties(devs[di], &qn, qp);
        for (uint32_t qi = 0; qi < qn; ++qi) {
            if (qp[qi].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                g_xpu_vk.phys = devs[di];
                g_xpu_vk.queueFamily = qi;
                found = 1;
                break;
            }
        }
    }
    if (!found) return 0;
    g_xpu_vk.vkGetPhysicalDeviceMemoryProperties(g_xpu_vk.phys,
                                                 &g_xpu_vk.memProps);

    // Ray-query probe: the BVH/ray-query path needs three device extensions
    // (acceleration_structure pulls in deferred_host_operations;
    // buffer_device_address backs the build's scratch/AABB addresses) AND the
    // matching feature bits. Enable them only when ALL are present — turning on
    // an unsupported extension fails vkCreateDevice outright, which would break
    // the plain compute path on a non-RT GPU. Absent any of them, rayQuery stays
    // 0 and the device is created exactly as before.
    int wantRayQuery = 0;
    if (enumDevExt && getFeatures2) {
        uint32_t extCount = 0;
        enumDevExt(g_xpu_vk.phys, NULL, &extCount, NULL);
        if (extCount > 0 && extCount <= 4096) {
            VkExtensionProperties* exts = (VkExtensionProperties*) malloc(
                sizeof(VkExtensionProperties) * extCount);
            if (exts) {
                enumDevExt(g_xpu_vk.phys, NULL, &extCount, exts);
                int hasAccel = 0, hasRayQ = 0, hasDefer = 0, hasBDA = 0;
                for (uint32_t i = 0; i < extCount; ++i) {
                    const char* en = exts[i].extensionName;
                    if (!strcmp(en, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) hasAccel = 1;
                    else if (!strcmp(en, VK_KHR_RAY_QUERY_EXTENSION_NAME)) hasRayQ = 1;
                    else if (!strcmp(en, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) hasDefer = 1;
                    else if (!strcmp(en, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) hasBDA = 1;
                }
                free(exts);
                if (hasAccel && hasRayQ && hasDefer && hasBDA) {
                    VkPhysicalDeviceRayQueryFeaturesKHR rqf;
                    memset(&rqf, 0, sizeof(rqf));
                    rqf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
                    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf;
                    memset(&asf, 0, sizeof(asf));
                    asf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
                    asf.pNext = &rqf;
                    VkPhysicalDeviceBufferDeviceAddressFeatures bdaf;
                    memset(&bdaf, 0, sizeof(bdaf));
                    bdaf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
                    bdaf.pNext = &asf;
                    VkPhysicalDeviceFeatures2 f2;
                    memset(&f2, 0, sizeof(f2));
                    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                    f2.pNext = &bdaf;
                    getFeatures2(g_xpu_vk.phys, &f2);
                    if (rqf.rayQuery && asf.accelerationStructure &&
                        bdaf.bufferDeviceAddress) {
                        wantRayQuery = 1;
                        // Cache the BVH-build scratch offset alignment.
                        if (getProps2) {
                            VkPhysicalDeviceAccelerationStructurePropertiesKHR asp;
                            memset(&asp, 0, sizeof(asp));
                            asp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
                            VkPhysicalDeviceProperties2 p2;
                            memset(&p2, 0, sizeof(p2));
                            p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                            p2.pNext = &asp;
                            getProps2(g_xpu_vk.phys, &p2);
                            g_xpu_vk.scratchAlign =
                                asp.minAccelerationStructureScratchOffsetAlignment;
                        }
                    }
                }
            }
        }
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g_xpu_vk.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    // Feature chain + extension list for the RT path (only when supported).
    const char* rtExts[4] = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    };
    VkPhysicalDeviceRayQueryFeaturesKHR enRqf;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enAsf;
    VkPhysicalDeviceBufferDeviceAddressFeatures enBdaf;
    if (wantRayQuery) {
        memset(&enRqf, 0, sizeof(enRqf));
        enRqf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        enRqf.rayQuery = VK_TRUE;
        memset(&enAsf, 0, sizeof(enAsf));
        enAsf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        enAsf.accelerationStructure = VK_TRUE;
        enAsf.pNext = &enRqf;
        memset(&enBdaf, 0, sizeof(enBdaf));
        enBdaf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        enBdaf.bufferDeviceAddress = VK_TRUE;
        enBdaf.pNext = &enAsf;
        dci.pNext = &enBdaf;
        dci.enabledExtensionCount = 4;
        dci.ppEnabledExtensionNames = rtExts;
    }

    // Enable shaderInt8 when supported: the software ray-query variant ($sw)
    // declares the SPIR-V Int8 capability (the SoftwareRayQuery walk uses
    // byte-width values), which Vulkan requires this feature for. Prepended to the
    // feature chain so it composes with the RT features above. Core in Vulkan 1.2+.
    VkPhysicalDeviceShaderFloat16Int8Features enInt8;
    memset(&enInt8, 0, sizeof(enInt8));
    enInt8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
    if (getFeatures2) {
        VkPhysicalDeviceShaderFloat16Int8Features q;
        memset(&q, 0, sizeof(q));
        q.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
        VkPhysicalDeviceFeatures2 qf2;
        memset(&qf2, 0, sizeof(qf2));
        qf2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        qf2.pNext = &q;
        getFeatures2(g_xpu_vk.phys, &qf2);
        if (q.shaderInt8) {
            enInt8.shaderInt8 = VK_TRUE;
            enInt8.pNext = (void*) dci.pNext;   // prepend to the existing chain
            dci.pNext = &enInt8;
        }
    }

    if (g_xpu_vk.vkCreateDevice(g_xpu_vk.phys, &dci, NULL, &g_xpu_vk.device)
            != VK_SUCCESS)
        return 0;

    #define CAJ_VKD(nm) g_xpu_vk.nm = (PFN_##nm)                               \
        g_xpu_vk.getDeviceProcAddr(g_xpu_vk.device, #nm)
    CAJ_VKD(vkGetDeviceQueue);
    CAJ_VKD(vkCreateBuffer);
    CAJ_VKD(vkDestroyBuffer);
    CAJ_VKD(vkGetBufferMemoryRequirements);
    CAJ_VKD(vkAllocateMemory);
    CAJ_VKD(vkFreeMemory);
    CAJ_VKD(vkBindBufferMemory);
    CAJ_VKD(vkMapMemory);
    CAJ_VKD(vkUnmapMemory);
    CAJ_VKD(vkCreateImage);
    CAJ_VKD(vkDestroyImage);
    CAJ_VKD(vkGetImageMemoryRequirements);
    CAJ_VKD(vkBindImageMemory);
    CAJ_VKD(vkCreateImageView);
    CAJ_VKD(vkDestroyImageView);
    CAJ_VKD(vkCreateSampler);
    CAJ_VKD(vkDestroySampler);
    CAJ_VKD(vkCmdCopyBufferToImage);
    CAJ_VKD(vkCmdCopyImageToBuffer);
    CAJ_VKD(vkCmdPipelineBarrier);
    CAJ_VKD(vkCreateShaderModule);
    CAJ_VKD(vkDestroyShaderModule);
    CAJ_VKD(vkCreateDescriptorSetLayout);
    CAJ_VKD(vkDestroyDescriptorSetLayout);
    CAJ_VKD(vkCreatePipelineLayout);
    CAJ_VKD(vkDestroyPipelineLayout);
    CAJ_VKD(vkCreateComputePipelines);
    CAJ_VKD(vkDestroyPipeline);
    CAJ_VKD(vkCreateDescriptorPool);
    CAJ_VKD(vkDestroyDescriptorPool);
    CAJ_VKD(vkAllocateDescriptorSets);
    CAJ_VKD(vkUpdateDescriptorSets);
    CAJ_VKD(vkCreateCommandPool);
    CAJ_VKD(vkDestroyCommandPool);
    CAJ_VKD(vkAllocateCommandBuffers);
    CAJ_VKD(vkFreeCommandBuffers);
    CAJ_VKD(vkBeginCommandBuffer);
    CAJ_VKD(vkEndCommandBuffer);
    CAJ_VKD(vkCmdBindPipeline);
    CAJ_VKD(vkCmdBindDescriptorSets);
    CAJ_VKD(vkCmdDispatch);
    CAJ_VKD(vkQueueSubmit);
    CAJ_VKD(vkQueueWaitIdle);
    // RT path: resolve the AS/device-address entry points only when the device
    // was created with the ray-query extensions. vkGetBufferDeviceAddress is
    // core 1.2; the AS builders are KHR. If any fails to resolve, drop back to
    // the plain compute path (rayQuery stays 0).
    if (wantRayQuery) {
        CAJ_VKD(vkGetBufferDeviceAddress);
        if (!g_xpu_vk.vkGetBufferDeviceAddress)
            // Core 1.2 name absent (e.g. a 1.1 device exposing only the KHR
            // extension): fall back to the ABI-identical KHR entry point.
            g_xpu_vk.vkGetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress)
                g_xpu_vk.getDeviceProcAddr(g_xpu_vk.device,
                                           "vkGetBufferDeviceAddressKHR");
        CAJ_VKD(vkGetAccelerationStructureBuildSizesKHR);
        CAJ_VKD(vkCreateAccelerationStructureKHR);
        CAJ_VKD(vkDestroyAccelerationStructureKHR);
        CAJ_VKD(vkCmdBuildAccelerationStructuresKHR);
        CAJ_VKD(vkGetAccelerationStructureDeviceAddressKHR);
        g_xpu_vk.rayQuery =
            g_xpu_vk.vkGetBufferDeviceAddress &&
            g_xpu_vk.vkGetAccelerationStructureBuildSizesKHR &&
            g_xpu_vk.vkCreateAccelerationStructureKHR &&
            g_xpu_vk.vkDestroyAccelerationStructureKHR &&
            g_xpu_vk.vkCmdBuildAccelerationStructuresKHR &&
            g_xpu_vk.vkGetAccelerationStructureDeviceAddressKHR ? 1 : 0;
    }
    #undef CAJ_VKD

    g_xpu_vk.vkGetDeviceQueue(g_xpu_vk.device, g_xpu_vk.queueFamily, 0,
                              &g_xpu_vk.queue);
    VkCommandPoolCreateInfo cpci;
    memset(&cpci, 0, sizeof(cpci));
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = g_xpu_vk.queueFamily;
    if (g_xpu_vk.vkCreateCommandPool(g_xpu_vk.device, &cpci, NULL,
                                     &g_xpu_vk.cmdPool) != VK_SUCCESS)
        return 0;
    g_xpu_vk.loaded = 1;
    return 1;
}

// Allocate a host-visible/coherent storage buffer; return a 1-based table
// handle (0 on failure). Reuses a dead slot if one is free.
static int64_t cajeta_xpu_vk_alloc(uint64_t bytes) {
    if (bytes == 0) return 0;
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    if (g_xpu_vk.vkCreateBuffer(g_xpu_vk.device, &bci, NULL, &buf) != VK_SUCCESS)
        return 0;
    VkMemoryRequirements req;
    memset(&req, 0, sizeof(req));
    g_xpu_vk.vkGetBufferMemoryRequirements(g_xpu_vk.device, buf, &req);
    int mt = cajeta_xpu_vk_find_memory_type(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) { g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL); return 0; }
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t) mt;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (g_xpu_vk.vkAllocateMemory(g_xpu_vk.device, &mai, NULL, &mem)
            != VK_SUCCESS) {
        g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL);
        return 0;
    }
    if (g_xpu_vk.vkBindBufferMemory(g_xpu_vk.device, buf, mem, 0) != VK_SUCCESS) {
        g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);   // L5: don't leave a
        g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL);// live unbacked slot
        return 0;
    }
    void* mapped = NULL;
    if (g_xpu_vk.vkMapMemory(g_xpu_vk.device, mem, 0, VK_WHOLE_SIZE, 0, &mapped)
            != VK_SUCCESS) {
        g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);
        g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL);
        return 0;
    }
    pthread_mutex_lock(&g_xpu_vk_submit_mu);   // g_vk_bufs table RMW
    int slot = -1;
    for (int i = 0; i < g_vk_buf_count; ++i)
        if (!g_vk_bufs[i].live) { slot = i; break; }
    if (slot < 0) {
        if (g_vk_buf_count >= CAJETA_VK_MAX_BUFFERS) {
            pthread_mutex_unlock(&g_xpu_vk_submit_mu);
            g_xpu_vk.vkUnmapMemory(g_xpu_vk.device, mem);
            g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);
            g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL);
            return 0;
        }
        slot = g_vk_buf_count++;
    }
    g_vk_bufs[slot].buffer = buf;
    g_vk_bufs[slot].memory = mem;
    g_vk_bufs[slot].mapped = mapped;
    g_vk_bufs[slot].size = bytes;
    g_vk_bufs[slot].live = 1;
    g_vk_bufs[slot].is_view = 0;        // an owner, not a slice view
    g_vk_bufs[slot].view_offset = 0;
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return (int64_t) (slot + 1);
}

// Buffer.slice on Vulkan: the handle is a buffer-table index, not a pointer, so
// the byte offset can't be folded into it. Instead allocate a *view* slot that
// borrows the parent's VkBuffer/VkDeviceMemory and records the byte offset; the
// descriptor-bind path emits it as VkDescriptorBufferInfo.offset and host
// transfers see it via the offset-folded `mapped`. The view never owns the
// underlying resources — freeing a view slot clears the slot only.
// NOTE: not yet device-verified (increment B). VkDescriptorBufferInfo.offset
// must be a multiple of minStorageBufferOffsetAlignment; a caller slicing at an
// unaligned element offset will need that handled when the Vulkan path is
// brought up on hardware.
static int64_t cajeta_xpu_vk_slice(int64_t parent, uint64_t byteOffset) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    struct cajeta_vk_buf* p = (parent > 0 && parent <= g_vk_buf_count &&
                               g_vk_bufs[parent - 1].live)
                                  ? &g_vk_bufs[parent - 1] : NULL;
    if (!p) { pthread_mutex_unlock(&g_xpu_vk_submit_mu); return 0; }
    VkDeviceSize base_off = p->view_offset + (VkDeviceSize) byteOffset;
    int slot = -1;
    for (int i = 0; i < g_vk_buf_count; ++i)
        if (!g_vk_bufs[i].live) { slot = i; break; }
    if (slot < 0) {
        if (g_vk_buf_count >= CAJETA_VK_MAX_BUFFERS) {
            pthread_mutex_unlock(&g_xpu_vk_submit_mu); return 0;
        }
        slot = g_vk_buf_count++;
    }
    g_vk_bufs[slot].buffer = p->buffer;          // borrowed, not owned
    g_vk_bufs[slot].memory = p->memory;
    g_vk_bufs[slot].mapped = p->mapped ? (void*) ((char*) p->mapped + base_off)
                                       : NULL;
    g_vk_bufs[slot].size = (p->size > base_off) ? p->size - base_off : 0;
    g_vk_bufs[slot].live = 1;
    g_vk_bufs[slot].is_view = 1;
    g_vk_bufs[slot].view_offset = base_off;
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return (int64_t) (slot + 1);
}

// rec/mapped/free take g_xpu_vk_submit_mu (recursive) so the table is read/written
// consistently even when called from a context that already holds it (vk_launch).
static struct cajeta_vk_buf* cajeta_xpu_vk_rec(int64_t handle) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    struct cajeta_vk_buf* r = NULL;
    if (handle > 0 && handle <= g_vk_buf_count && g_vk_bufs[handle - 1].live)
        r = &g_vk_bufs[handle - 1];
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return r;
}
static void* cajeta_xpu_vk_mapped(int64_t handle) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(handle);
    void* m = r ? r->mapped : NULL;
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return m;
}
static void cajeta_xpu_vk_free(int64_t handle) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(handle);
    if (r) {
        if (r->is_view) {
            // A slice view borrows the parent's buffer/memory — clear the slot
            // only; the parent (its owner) destroys the resources.
            r->live = 0; r->mapped = NULL; r->buffer = VK_NULL_HANDLE;
            r->memory = VK_NULL_HANDLE; r->is_view = 0; r->view_offset = 0;
        } else {
            if (r->mapped) g_xpu_vk.vkUnmapMemory(g_xpu_vk.device, r->memory);
            if (r->buffer) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, r->buffer, NULL);
            if (r->memory) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, r->memory, NULL);
            r->live = 0; r->mapped = NULL; r->buffer = VK_NULL_HANDLE;
            r->memory = VK_NULL_HANDLE;
        }
    }
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
}

// --- Vulkan sampled-image (Texture2D) table (Item 8 Stage B) ----------------
// A Texture2D's device handle on Vulkan is a 1-based index into this table. The
// image is R32_SFLOAT (single-channel float, matching the scalar texel), OPTIMAL
// tiled + device-local, used as SAMPLED_IMAGE. Texels are staged through a
// host-visible buffer + copy with layout transitions on upload.
struct cajeta_vk_tex {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    uint32_t w, h, d;       // d = depth: 1 for a 2-D image, >=1 for a 3-D image
    uint32_t layers;        // array-layer count (N for 2D-array, 6 for cube, else 1)
    int layered;            // 1 if the layers are array layers (2D-array/cube) — the
                            // upload copies them via subresource layerCount, not depth
    int live;
    int storage;            // 1 = writable STORAGE_IMAGE (Image2D), 0 = sampled
    int32_t format;         // TextureFormat ordinal (sampled images; storage = R32F)
    VkImageLayout layout;   // current layout (tracked for storage-image barriers)
};
#define CAJETA_VK_MAX_TEXTURES 256
static struct cajeta_vk_tex g_vk_texs[CAJETA_VK_MAX_TEXTURES];
static int g_vk_tex_count;

static struct cajeta_vk_tex* cajeta_xpu_vk_tex_rec(int64_t handle) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);   // g_vk_texs read (recursive)
    struct cajeta_vk_tex* t = NULL;
    if (handle > 0 && handle <= g_vk_tex_count && g_vk_texs[handle - 1].live)
        t = &g_vk_texs[handle - 1];
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return t;
}

// Create a 2-D R32_SFLOAT image + view; return a 1-based table handle (0 on
// failure). `storage`=0 makes a SAMPLED image (Texture2D; contents undefined
// until cajeta_xpu_vk_tex_upload). `storage`=1 makes a writable STORAGE_IMAGE
// (Image2D) usable as an OpImageWrite target and readable back to the host
// (TRANSFER_SRC) — its texels start undefined and are produced by a kernel.
// `imageKind` is the texture kind (1/2/3/4/5 = 1D/2D/3D/2D-array/cube) — the
// single axis selecting the image + view type, the used extent components, and
// whether `arrayLayers` are array layers (2D-array/cube) vs a true depth (3D). A
// 1-D image has h = depth = 1; a 2-D image has depth = 1; a 2D-array/cube has
// depth = 1 and arrayLayers > 1 (cube = 6, with the CUBE_COMPATIBLE flag).
static int64_t cajeta_xpu_vk_tex_alloc(uint32_t w, uint32_t h, int storage,
                                       int32_t format, uint32_t depth, int imageKind,
                                       uint32_t arrayLayers, uint32_t mipLevels) {
    if (w == 0 || h == 0 || depth == 0) return 0;
    if (arrayLayers == 0) arrayLayers = 1;
    int is3d = (imageKind == 3);
    int isCube = (imageKind == 5);
    int layered = (imageKind == 4 || imageKind == 5);   // array layers, not depth
    if (mipLevels == 0) mipLevels = 1;
    // Storage images (Image2D) are R32F only; sampled images (Texture2D) pick a
    // VkFormat from the TextureFormat ordinal. All sample to float in the shader,
    // so the descriptor format is the only thing that varies.
    VkFormat vkfmt = VK_FORMAT_R32_SFLOAT;
    if (!storage) {
        switch (format) {
            case CAJ_TEXFMT_R8_UNORM:    vkfmt = VK_FORMAT_R8_UNORM;            break;
            case CAJ_TEXFMT_RGBA8_UNORM: vkfmt = VK_FORMAT_R8G8B8A8_UNORM;      break;
            case CAJ_TEXFMT_RGBA32F:     vkfmt = VK_FORMAT_R32G32B32A32_SFLOAT; break;
            case CAJ_TEXFMT_R16F:        vkfmt = VK_FORMAT_R16_SFLOAT;          break;
            case CAJ_TEXFMT_RGBA16F:     vkfmt = VK_FORMAT_R16G16B16A16_SFLOAT; break;
            case CAJ_TEXFMT_R32I:        vkfmt = VK_FORMAT_R32_SINT;            break;
            case CAJ_TEXFMT_R32UI:       vkfmt = VK_FORMAT_R32_UINT;            break;
            case CAJ_TEXFMT_RGBA32I:     vkfmt = VK_FORMAT_R32G32B32A32_SINT;   break;
            case CAJ_TEXFMT_RGBA32UI:    vkfmt = VK_FORMAT_R32G32B32A32_UINT;   break;
            case CAJ_TEXFMT_R32F: default: vkfmt = VK_FORMAT_R32_SFLOAT;        break;
        }
    }
    VkImageCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    // 2D-array and cube are both 2-D image types (the layering is in arrayLayers
    // + the view type); only a cube needs the CUBE_COMPATIBLE create flag.
    ici.imageType = is3d ? VK_IMAGE_TYPE_3D
                         : (imageKind == 1 ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D);
    ici.flags = isCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ici.format = vkfmt;
    ici.extent.width = w; ici.extent.height = h;
    ici.extent.depth = is3d ? depth : 1;
    ici.mipLevels = mipLevels;
    ici.arrayLayers = layered ? arrayLayers : 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = storage
        ? (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
           VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        : (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    if (g_xpu_vk.vkCreateImage(g_xpu_vk.device, &ici, NULL, &img) != VK_SUCCESS)
        return 0;
    VkMemoryRequirements req;
    memset(&req, 0, sizeof(req));
    g_xpu_vk.vkGetImageMemoryRequirements(g_xpu_vk.device, img, &req);
    int mt = cajeta_xpu_vk_find_memory_type(req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0)   // no device-local: any type works (host-visible is acceptable)
        mt = cajeta_xpu_vk_find_memory_type(req.memoryTypeBits, 0);
    if (mt < 0) { g_xpu_vk.vkDestroyImage(g_xpu_vk.device, img, NULL); return 0; }
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t) mt;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (g_xpu_vk.vkAllocateMemory(g_xpu_vk.device, &mai, NULL, &mem)
            != VK_SUCCESS) {
        g_xpu_vk.vkDestroyImage(g_xpu_vk.device, img, NULL); return 0;
    }
    if (g_xpu_vk.vkBindImageMemory(g_xpu_vk.device, img, mem, 0) != VK_SUCCESS) {
        g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);   // L5
        g_xpu_vk.vkDestroyImage(g_xpu_vk.device, img, NULL);
        return 0;
    }
    VkImageViewCreateInfo vci;
    memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = img;
    vci.viewType = is3d ? VK_IMAGE_VIEW_TYPE_3D
                 : (imageKind == 1 ? VK_IMAGE_VIEW_TYPE_1D
                 : (imageKind == 4 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                 : (isCube ? VK_IMAGE_VIEW_TYPE_CUBE
                           : VK_IMAGE_VIEW_TYPE_2D)));
    vci.format = vkfmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = mipLevels;
    vci.subresourceRange.layerCount = layered ? arrayLayers : 1;
    VkImageView view = VK_NULL_HANDLE;
    if (g_xpu_vk.vkCreateImageView(g_xpu_vk.device, &vci, NULL, &view)
            != VK_SUCCESS) {
        g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);
        g_xpu_vk.vkDestroyImage(g_xpu_vk.device, img, NULL);
        return 0;
    }
    pthread_mutex_lock(&g_xpu_vk_submit_mu);   // g_vk_texs table RMW
    int slot = -1;
    for (int i = 0; i < g_vk_tex_count; ++i)
        if (!g_vk_texs[i].live) { slot = i; break; }
    if (slot < 0) {
        if (g_vk_tex_count >= CAJETA_VK_MAX_TEXTURES) {
            pthread_mutex_unlock(&g_xpu_vk_submit_mu);
            g_xpu_vk.vkDestroyImageView(g_xpu_vk.device, view, NULL);
            g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);
            g_xpu_vk.vkDestroyImage(g_xpu_vk.device, img, NULL);
            return 0;
        }
        slot = g_vk_tex_count++;
    }
    g_vk_texs[slot].image = img;
    g_vk_texs[slot].memory = mem;
    g_vk_texs[slot].view = view;
    g_vk_texs[slot].w = w; g_vk_texs[slot].h = h;
    g_vk_texs[slot].d = is3d ? depth : 1;
    g_vk_texs[slot].layers = layered ? arrayLayers : 1;
    g_vk_texs[slot].layered = layered;
    g_vk_texs[slot].live = 1;
    g_vk_texs[slot].storage = storage;
    g_vk_texs[slot].format = storage ? CAJ_TEXFMT_R32F : format;
    g_vk_texs[slot].layout = VK_IMAGE_LAYOUT_UNDEFINED;
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return (int64_t) (slot + 1);
}

// Copy `src` (lw*lh*ld*layers row-major float32 texels) into mip `level` of image
// `t`, leaving that subresource in SHADER_READ_ONLY_OPTIMAL ready to sample.
// `layers` is the array-layer count (1 for a plain 2-D/3-D image; N for a 2-D
// array; 6 for a cube) — the layers are laid out contiguously after the (lw*lh*ld)
// plane, copied via the subresource layerCount (NOT extent.depth). Transient
// host-visible staging buffer + a one-time command buffer (barrier / copy /
// barrier) on the shared VkQueue (this routine takes g_xpu_vk_submit_mu itself).
// Returns 1 on success, 0 if the command buffer couldn't be recorded/submitted
// (the subresource is left uninitialized). The per-level barriers use
// baseMipLevel=level/levelCount=1, so each level is transitioned independently —
// uploading every level of a mip chain leaves the whole image SHADER_READ.
static int cajeta_xpu_vk_tex_copy_region(struct cajeta_vk_tex* t, const float* src,
                                         uint32_t lw, uint32_t lh, uint32_t ld,
                                         uint32_t layers, uint32_t level,
                                         int32_t format) {
    if (!t || !src || lw == 0 || lh == 0 || ld == 0) return 0;
    if (layers == 0) layers = 1;
    size_t texels =
        (size_t) lw * lh * ld * layers * cajeta_texfmt_channels(format);
    uint64_t bytes =
        (uint64_t) lw * lh * ld * layers * cajeta_texfmt_texel_bytes(format);
    int64_t staging = cajeta_xpu_vk_alloc(bytes);   // host-visible+coherent
    if (!staging) return 0;
    void* m = cajeta_xpu_vk_mapped(staging);
    if (m) cajeta_texfmt_encode(m, src, texels, format);   // float memcpy / UNORM quantize
    struct cajeta_vk_buf* sb = cajeta_xpu_vk_rec(staging);

    // The staging copy submits to the shared VkQueue/VkCommandPool — serialize it
    // against concurrent launches / AS builds (same external-sync requirement).
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_xpu_vk.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    int ok = 0;
    if (g_xpu_vk.vkAllocateCommandBuffers(g_xpu_vk.device, &cbai, &cmd)
            == VK_SUCCESS && sb) {
        ok = 1;
        VkCommandBufferBeginInfo cbbi;
        memset(&cbbi, 0, sizeof(cbbi));
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        g_xpu_vk.vkBeginCommandBuffer(cmd, &cbbi);

        VkImageMemoryBarrier toDst;
        memset(&toDst, 0, sizeof(toDst));
        toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = t->image;
        toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toDst.subresourceRange.baseMipLevel = level;
        toDst.subresourceRange.levelCount = 1;
        toDst.subresourceRange.layerCount = layers;
        toDst.srcAccessMask = 0;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        g_xpu_vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                      0, NULL, 0, NULL, 1, &toDst);

        VkBufferImageCopy region;
        memset(&region, 0, sizeof(region));
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = level;
        region.imageSubresource.layerCount = layers;
        region.imageExtent.width = lw;
        region.imageExtent.height = lh;
        region.imageExtent.depth = ld;
        g_xpu_vk.vkCmdCopyBufferToImage(cmd, sb->buffer, t->image,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        1, &region);

        VkImageMemoryBarrier toRead = toDst;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        g_xpu_vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                      0, NULL, 0, NULL, 1, &toRead);

        g_xpu_vk.vkEndCommandBuffer(cmd);
        VkSubmitInfo si;
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        g_xpu_vk.vkQueueSubmit(g_xpu_vk.queue, 1, &si, VK_NULL_HANDLE);
        g_xpu_vk.vkQueueWaitIdle(g_xpu_vk.queue);
        g_xpu_vk.vkFreeCommandBuffers(g_xpu_vk.device, g_xpu_vk.cmdPool, 1, &cmd);
    }
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    cajeta_xpu_vk_free(staging);
    return ok;
}

// Stage `data` (w*h row-major float32 texels) into mip level 0, leaving it in
// SHADER_READ_ONLY_OPTIMAL ready to sample.
static void cajeta_xpu_vk_tex_upload(int64_t handle, const float* src,
                                     uint32_t w, uint32_t h, int32_t format) {
    struct cajeta_vk_tex* t = cajeta_xpu_vk_tex_rec(handle);
    if (!t || !src || w != t->w || h != t->h) return;
    // A layered image (2-D array / cube) carries its planes in array layers, not
    // depth: copy depth 1 × N layers. A 3-D image carries them in depth × 1 layer.
    uint32_t dd = t->layered ? 1 : (t->d ? t->d : 1);
    uint32_t ll = t->layered ? (t->layers ? t->layers : 1) : 1;
    // M7: if the upload couldn't be recorded the image stays UNDEFINED but a
    // later launch binds it as SHADER_READ_ONLY_OPTIMAL — surface, don't fail
    // silently.
    if (!cajeta_xpu_vk_tex_copy_region(t, src, w, h, dd, ll, 0, format))
        fprintf(stderr, "cajeta.xpu: texture upload could not record/submit "
                "(handle %lld); the image is left uninitialized\n",
                (long long) handle);
}

// Stage one mip level: `src` is lw*lh row-major float32 texels for `level`
// (depth 1 — mip Texture2D only). Each level is an independent copy_region.
static void cajeta_xpu_vk_tex_upload_level(int64_t handle, const float* src,
                                           uint32_t lw, uint32_t lh,
                                           uint32_t level, int32_t format) {
    struct cajeta_vk_tex* t = cajeta_xpu_vk_tex_rec(handle);
    if (!t || !src) return;
    if (!cajeta_xpu_vk_tex_copy_region(t, src, lw, lh, 1, 1, level, format))
        fprintf(stderr, "cajeta.xpu: texture mip-level upload could not "
                "record/submit (handle %lld, level %u)\n",
                (long long) handle, level);
}

static void cajeta_xpu_vk_tex_free(int64_t handle) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);   // serialize vs launch + table
    struct cajeta_vk_tex* t = cajeta_xpu_vk_tex_rec(handle);
    if (t) {
        if (t->view) g_xpu_vk.vkDestroyImageView(g_xpu_vk.device, t->view, NULL);
        if (t->image) g_xpu_vk.vkDestroyImage(g_xpu_vk.device, t->image, NULL);
        if (t->memory) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, t->memory, NULL);
        t->live = 0; t->image = VK_NULL_HANDLE; t->view = VK_NULL_HANDLE;
        t->memory = VK_NULL_HANDLE;
    }
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
}

// Read a storage image (Image2D) back to host memory: w*h row-major float32
// texels into `data`. After a kernel's OpImageWrite the image is in GENERAL
// layout; transition it to TRANSFER_SRC, copy to a host-visible staging buffer,
// and memcpy out. Mirrors cajeta_xpu_vk_tex_upload in reverse (one-time command
// buffer, serialized on the shared queue). The image is left in TRANSFER_SRC
// (its tracked layout is updated, so a subsequent dispatch re-barriers to GENERAL).
static void cajeta_xpu_vk_tex_download(int64_t handle, void* data,
                                       uint32_t w, uint32_t h) {
    struct cajeta_vk_tex* t = cajeta_xpu_vk_tex_rec(handle);
    if (!t || !data || w != t->w || h != t->h) return;
    uint64_t bytes = (uint64_t) w * h * sizeof(float);
    int64_t staging = cajeta_xpu_vk_alloc(bytes);   // host-visible+coherent
    if (!staging) return;
    struct cajeta_vk_buf* sb = cajeta_xpu_vk_rec(staging);

    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_xpu_vk.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    int copied = 0;
    if (g_xpu_vk.vkAllocateCommandBuffers(g_xpu_vk.device, &cbai, &cmd)
            == VK_SUCCESS && sb) {
        copied = 1;
        VkCommandBufferBeginInfo cbbi;
        memset(&cbbi, 0, sizeof(cbbi));
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        g_xpu_vk.vkBeginCommandBuffer(cmd, &cbbi);

        VkImageMemoryBarrier toSrc;
        memset(&toSrc, 0, sizeof(toSrc));
        toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSrc.oldLayout = t->layout;   // GENERAL after a write (or UNDEFINED)
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image = t->image;
        toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toSrc.subresourceRange.levelCount = 1;
        toSrc.subresourceRange.layerCount = 1;
        toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        g_xpu_vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                      0, NULL, 0, NULL, 1, &toSrc);

        VkBufferImageCopy region;
        memset(&region, 0, sizeof(region));
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = w;
        region.imageExtent.height = h;
        region.imageExtent.depth = 1;
        g_xpu_vk.vkCmdCopyImageToBuffer(cmd, t->image,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        sb->buffer, 1, &region);

        g_xpu_vk.vkEndCommandBuffer(cmd);
        VkSubmitInfo si;
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        g_xpu_vk.vkQueueSubmit(g_xpu_vk.queue, 1, &si, VK_NULL_HANDLE);
        g_xpu_vk.vkQueueWaitIdle(g_xpu_vk.queue);
        g_xpu_vk.vkFreeCommandBuffers(g_xpu_vk.device, g_xpu_vk.cmdPool, 1, &cmd);
        t->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    if (copied) {
        void* m = cajeta_xpu_vk_mapped(staging);
        if (m) memcpy(data, m, (size_t) bytes);   // host-coherent: no flush
    }
    cajeta_xpu_vk_free(staging);
}

// Create a transient VkSampler from a cajeta Sampler's modes: filterMode 0 =
// nearest, 1 = linear; addressMode 0 = clamp-to-edge, 1 = repeat. Normalized
// coords (unnormalizedCoordinates = FALSE); single mip (sample at LOD 0).
// Returns the VkSampler as an int64 (0 on failure) so the build-shared launch
// translation never names a Vulkan type. Pair with cajeta_xpu_vk_destroy_sampler.
static int64_t cajeta_xpu_vk_make_sampler(int32_t filterMode,
                                          int32_t addressMode) {
    VkFilter f = filterMode == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    VkSamplerAddressMode a = addressMode == 1
                                 ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                 : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSamplerCreateInfo sci;
    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = f; sci.minFilter = f;
    sci.mipmapMode = filterMode == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                     : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = a; sci.addressModeV = a; sci.addressModeW = a;
    // maxLod must admit the highest mip level an explicit-LOD sample can request;
    // 0.0 clamps every LOD to level 0 (so sampleLod(.., lod>0) never reaches the
    // smaller mips). VK_LOD_CLAMP_NONE (1000.0) imposes no clamp — single-level
    // (non-mip) Texture2D is unaffected (only level 0 exists to sample).
    sci.minLod = 0.0f; sci.maxLod = VK_LOD_CLAMP_NONE;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    sci.unnormalizedCoordinates = VK_FALSE;
    VkSampler s = VK_NULL_HANDLE;
    if (g_xpu_vk.vkCreateSampler(g_xpu_vk.device, &sci, NULL, &s) != VK_SUCCESS)
        return 0;
    return (int64_t) (uintptr_t) s;
}

static void cajeta_xpu_vk_destroy_sampler(int64_t handle) {
    if (!handle) return;
    g_xpu_vk.vkDestroySampler(g_xpu_vk.device,
                              (VkSampler) (uintptr_t) handle, NULL);
}

// --- Vulkan acceleration-structure (BVH) table (Part C inc 3b) ---------------
// An AccelerationStructure's device handle on Vulkan is a 1-based index into
// this table. v1 builds a single bottom-level AS over AABB (procedural) geometry
// — the spatial-index primitive the RayQuery walks. All build inputs/scratch are
// device-address buffers (VK_KHR_buffer_device_address); the AS itself is bound
// in a kernel as VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR (see the launch
// path). Only reached when g_xpu_vk.rayQuery == 1.
struct cajeta_vk_accel {
    VkAccelerationStructureKHR accel;
    VkBuffer asBuf;          // AS backing store (must outlive the AS)
    VkDeviceMemory asMem;
    int live;
};
#define CAJETA_VK_MAX_ACCELS 256
static struct cajeta_vk_accel g_vk_accels[CAJETA_VK_MAX_ACCELS];
static int g_vk_accel_count;

static struct cajeta_vk_accel* cajeta_xpu_vk_accel_rec(int64_t handle) {
    if (handle <= 0 || handle > g_vk_accel_count) return NULL;
    struct cajeta_vk_accel* a = &g_vk_accels[handle - 1];
    return a->live ? a : NULL;
}

// Create a buffer that exposes a device address (VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
// + SHADER_DEVICE_ADDRESS usage), backed by memory satisfying `props`. Used for the
// AS build input/scratch/store. Returns 1 on success. `outMapped` non-NULL means the
// caller will fill the buffer from the host, so `props` MUST be host-visible (no
// fallback — a non-mappable type would break the memcpy). For a device-only buffer
// (`outMapped` NULL), if `props` (e.g. DEVICE_LOCAL) isn't available we fall back to
// any memory type — correctness over the device-local perf preference.
static int cajeta_xpu_vk_make_addr_buffer(uint64_t bytes, VkBufferUsageFlags usage,
                                          VkMemoryPropertyFlags props,
                                          VkBuffer* outBuf, VkDeviceMemory* outMem,
                                          void** outMapped) {
    if (bytes == 0) return 0;
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    if (g_xpu_vk.vkCreateBuffer(g_xpu_vk.device, &bci, NULL, &buf) != VK_SUCCESS)
        return 0;
    VkMemoryRequirements req;
    memset(&req, 0, sizeof(req));
    g_xpu_vk.vkGetBufferMemoryRequirements(g_xpu_vk.device, buf, &req);
    int mt = cajeta_xpu_vk_find_memory_type(req.memoryTypeBits, props);
    if (mt < 0 && !outMapped)   // device-only buffer: any memory type is fine
        mt = cajeta_xpu_vk_find_memory_type(req.memoryTypeBits, 0);
    if (mt < 0) { g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL); return 0; }
    VkMemoryAllocateFlagsInfo fi;
    memset(&fi, 0, sizeof(fi));
    fi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &fi;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t) mt;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (g_xpu_vk.vkAllocateMemory(g_xpu_vk.device, &mai, NULL, &mem) != VK_SUCCESS) {
        g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL);
        return 0;
    }
    if (g_xpu_vk.vkBindBufferMemory(g_xpu_vk.device, buf, mem, 0) != VK_SUCCESS) {
        g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);   // L5
        g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL);
        return 0;
    }
    if (outMapped) {
        *outMapped = NULL;
        if (g_xpu_vk.vkMapMemory(g_xpu_vk.device, mem, 0, VK_WHOLE_SIZE, 0,
                                 outMapped) != VK_SUCCESS) {
            g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);
            g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL);
            return 0;
        }
    }
    *outBuf = buf;
    *outMem = mem;
    return 1;
}

static VkDeviceAddress cajeta_xpu_vk_buf_addr(VkBuffer b) {
    VkBufferDeviceAddressInfo i;
    memset(&i, 0, sizeof(i));
    i.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    i.buffer = b;
    return g_xpu_vk.vkGetBufferDeviceAddress(g_xpu_vk.device, &i);
}

// Build a bottom-level AS over `count` AABBs, each packed as 6 float32
// (minX,minY,minZ,maxX,maxY,maxZ) — byte-identical to VkAabbPositionsKHR, so the
// cajeta float32[] uploads straight in. Returns a 1-based table handle (0 on
// failure / no RT device). The AS + its backing store outlive this call; the
// AABB input and scratch are transient and freed before returning.
static int64_t cajeta_xpu_vk_accel_build_aabbs(const float* aabbs, uint32_t count) {
    if (!g_xpu_vk.rayQuery || !aabbs || count == 0) return 0;

    // Serialize the whole build: it submits to the shared queue/cmdpool and
    // mutates the g_vk_accels table (slot-find + count++), both of which race
    // a concurrent launch/build/free from another OS thread without this.
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    VkBuffer aabbBuf = VK_NULL_HANDLE, asBuf = VK_NULL_HANDLE,
             scratchBuf = VK_NULL_HANDLE;
    VkDeviceMemory aabbMem = VK_NULL_HANDLE, asMem = VK_NULL_HANDLE,
                   scratchMem = VK_NULL_HANDLE;
    VkAccelerationStructureKHR accel = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    int64_t result = 0;

    // 1. AABB input buffer (count * 24 bytes), filled from the host.
    uint64_t aabbBytes = (uint64_t) count * sizeof(VkAabbPositionsKHR);
    void* aabbMapped = NULL;
    if (!cajeta_xpu_vk_make_addr_buffer(
            aabbBytes,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &aabbBuf, &aabbMem, &aabbMapped))   // host-mapped: filled by memcpy
        goto accel_done;
    memcpy(aabbMapped, aabbs, (size_t) aabbBytes);

    // 2. Geometry descriptor (opaque procedural AABBs).
    VkAccelerationStructureGeometryKHR geom;
    memset(&geom, 0, sizeof(geom));
    geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geom.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geom.geometry.aabbs.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
    geom.geometry.aabbs.stride = sizeof(VkAabbPositionsKHR);
    geom.geometry.aabbs.data.deviceAddress = cajeta_xpu_vk_buf_addr(aabbBuf);

    // 3. Build-geometry info (sizes query needs geometry but not yet dst/scratch).
    VkAccelerationStructureBuildGeometryInfoKHR bgi;
    memset(&bgi, 0, sizeof(bgi));
    bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bgi.geometryCount = 1;
    bgi.pGeometries = &geom;

    uint32_t primCount = count;
    VkAccelerationStructureBuildSizesInfoKHR sizes;
    memset(&sizes, 0, sizeof(sizes));
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    g_xpu_vk.vkGetAccelerationStructureBuildSizesKHR(
        g_xpu_vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bgi,
        &primCount, &sizes);
    if (sizes.accelerationStructureSize == 0 || sizes.buildScratchSize == 0)
        goto accel_done;

    // 4. AS backing store + the AS object over it.
    if (!cajeta_xpu_vk_make_addr_buffer(
            sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,   // device-only (host never touches)
            &asBuf, &asMem, NULL))
        goto accel_done;
    VkAccelerationStructureCreateInfoKHR aci;
    memset(&aci, 0, sizeof(aci));
    aci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    aci.buffer = asBuf;
    aci.size = sizes.accelerationStructureSize;
    aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (g_xpu_vk.vkCreateAccelerationStructureKHR(g_xpu_vk.device, &aci, NULL,
                                                  &accel) != VK_SUCCESS)
        goto accel_done;

    // 5. Scratch buffer; complete the build-geometry info. The scratch device
    // address MUST be aligned to minAccelerationStructureScratchOffsetAlignment
    // (VUID-VkAccelerationStructureBuildGeometryInfoKHR-scratchData-03710) — a raw
    // buffer base is not guaranteed to satisfy it, so over-allocate by (align-1)
    // and round the address up. scratchAlign is a power of two (Vulkan requires it).
    VkDeviceSize scratchAlign = g_xpu_vk.scratchAlign ? g_xpu_vk.scratchAlign : 256;
    if (!cajeta_xpu_vk_make_addr_buffer(sizes.buildScratchSize + scratchAlign - 1,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                        &scratchBuf, &scratchMem, NULL))
        goto accel_done;
    bgi.dstAccelerationStructure = accel;
    {
        VkDeviceAddress sAddr = cajeta_xpu_vk_buf_addr(scratchBuf);
        sAddr = (sAddr + scratchAlign - 1) & ~((VkDeviceAddress) scratchAlign - 1);
        bgi.scratchData.deviceAddress = sAddr;
    }

    // 6. Record + submit the build.
    VkAccelerationStructureBuildRangeInfoKHR range;
    memset(&range, 0, sizeof(range));
    range.primitiveCount = count;
    const VkAccelerationStructureBuildRangeInfoKHR* pranges = &range;

    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_xpu_vk.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (g_xpu_vk.vkAllocateCommandBuffers(g_xpu_vk.device, &cbai, &cmd)
            != VK_SUCCESS)
        goto accel_done;
    VkCommandBufferBeginInfo cbbi;
    memset(&cbbi, 0, sizeof(cbbi));
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_xpu_vk.vkBeginCommandBuffer(cmd, &cbbi);
    g_xpu_vk.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bgi, &pranges);
    g_xpu_vk.vkEndCommandBuffer(cmd);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (g_xpu_vk.vkQueueSubmit(g_xpu_vk.queue, 1, &si, VK_NULL_HANDLE)
            != VK_SUCCESS)
        goto accel_done;
    g_xpu_vk.vkQueueWaitIdle(g_xpu_vk.queue);

    // 7. Record the AS in the table (asBuf/asMem/accel survive; clear locals so
    // the cleanup below doesn't tear them down).
    {
        int slot = -1;
        for (int i = 0; i < g_vk_accel_count; ++i)
            if (!g_vk_accels[i].live) { slot = i; break; }
        if (slot < 0) {
            if (g_vk_accel_count >= CAJETA_VK_MAX_ACCELS) goto accel_done;
            slot = g_vk_accel_count++;
        }
        g_vk_accels[slot].accel = accel;
        g_vk_accels[slot].asBuf = asBuf;
        g_vk_accels[slot].asMem = asMem;
        g_vk_accels[slot].live = 1;
        result = (int64_t) (slot + 1);
        accel = VK_NULL_HANDLE; asBuf = VK_NULL_HANDLE; asMem = VK_NULL_HANDLE;
    }

accel_done:
    if (cmd) g_xpu_vk.vkFreeCommandBuffers(g_xpu_vk.device, g_xpu_vk.cmdPool, 1,
                                           &cmd);
    if (scratchBuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, scratchBuf, NULL);
    if (scratchMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, scratchMem, NULL);
    if (aabbMapped) g_xpu_vk.vkUnmapMemory(g_xpu_vk.device, aabbMem);
    if (aabbBuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, aabbBuf, NULL);
    if (aabbMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, aabbMem, NULL);
    // On failure these are still set (success cleared them); tear them down.
    if (accel) g_xpu_vk.vkDestroyAccelerationStructureKHR(g_xpu_vk.device, accel,
                                                          NULL);
    if (asBuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, asBuf, NULL);
    if (asMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, asMem, NULL);
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return result;
}

// Triangle BLAS twin of cajeta_xpu_vk_accel_build_aabbs: a bottom-level AS over
// `triCount` triangles from a vertex soup (`stride` floats per vertex; 3 = tight).
// Non-indexed (VK_INDEX_TYPE_NONE_KHR): vertexCount = triCount*3, primCount =
// triCount. The vertex buffer is a transient build input (freed after the build);
// only the AS backing store survives, exactly like the AABB path.
static int64_t cajeta_xpu_vk_accel_build_triangles(const float* verts,
                                                   uint32_t triCount,
                                                   uint32_t stride) {
    if (!g_xpu_vk.rayQuery || !verts || triCount == 0 || stride < 3u) return 0;

    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    VkBuffer vbuf = VK_NULL_HANDLE, asBuf = VK_NULL_HANDLE,
             scratchBuf = VK_NULL_HANDLE;
    VkDeviceMemory vmem = VK_NULL_HANDLE, asMem = VK_NULL_HANDLE,
                   scratchMem = VK_NULL_HANDLE;
    VkAccelerationStructureKHR accel = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    int64_t result = 0;

    // 1. Vertex input buffer (triCount*3 vertices, `stride` floats each).
    uint32_t vertexCount = triCount * 3u;
    uint64_t vBytes = (uint64_t) vertexCount * stride * sizeof(float);
    void* vMapped = NULL;
    if (!cajeta_xpu_vk_make_addr_buffer(
            vBytes,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &vbuf, &vmem, &vMapped))
        goto tri_done;
    memcpy(vMapped, verts, (size_t) vBytes);

    // 2. Triangle geometry descriptor.
    VkAccelerationStructureGeometryKHR geom;
    memset(&geom, 0, sizeof(geom));
    geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    // Non-opaque: the ray-query `proceed()` loop ENUMERATES each triangle hit as a
    // candidate (candidateType 0), matching the software walk's enumerate-all model
    // (the software path has no commit yet — confirm/generate is inc 3). Opaque
    // triangles would auto-commit and never surface as candidates in the loop.
    geom.flags = 0;
    geom.geometry.triangles.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geom.geometry.triangles.vertexData.deviceAddress = cajeta_xpu_vk_buf_addr(vbuf);
    geom.geometry.triangles.vertexStride = (VkDeviceSize) stride * sizeof(float);
    geom.geometry.triangles.maxVertex = vertexCount - 1u;
    geom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;

    // 3. Sizes.
    VkAccelerationStructureBuildGeometryInfoKHR bgi;
    memset(&bgi, 0, sizeof(bgi));
    bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bgi.geometryCount = 1;
    bgi.pGeometries = &geom;

    uint32_t primCount = triCount;
    VkAccelerationStructureBuildSizesInfoKHR sizes;
    memset(&sizes, 0, sizeof(sizes));
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    g_xpu_vk.vkGetAccelerationStructureBuildSizesKHR(
        g_xpu_vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bgi,
        &primCount, &sizes);
    if (sizes.accelerationStructureSize == 0 || sizes.buildScratchSize == 0)
        goto tri_done;

    // 4. AS backing store + object.
    if (!cajeta_xpu_vk_make_addr_buffer(
            sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &asBuf, &asMem, NULL))
        goto tri_done;
    VkAccelerationStructureCreateInfoKHR aci;
    memset(&aci, 0, sizeof(aci));
    aci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    aci.buffer = asBuf;
    aci.size = sizes.accelerationStructureSize;
    aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (g_xpu_vk.vkCreateAccelerationStructureKHR(g_xpu_vk.device, &aci, NULL,
                                                  &accel) != VK_SUCCESS)
        goto tri_done;

    // 5. Scratch (aligned).
    VkDeviceSize scratchAlign = g_xpu_vk.scratchAlign ? g_xpu_vk.scratchAlign : 256;
    if (!cajeta_xpu_vk_make_addr_buffer(sizes.buildScratchSize + scratchAlign - 1,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                        &scratchBuf, &scratchMem, NULL))
        goto tri_done;
    bgi.dstAccelerationStructure = accel;
    {
        VkDeviceAddress sAddr = cajeta_xpu_vk_buf_addr(scratchBuf);
        sAddr = (sAddr + scratchAlign - 1) & ~((VkDeviceAddress) scratchAlign - 1);
        bgi.scratchData.deviceAddress = sAddr;
    }

    // 6. Record + submit.
    VkAccelerationStructureBuildRangeInfoKHR range;
    memset(&range, 0, sizeof(range));
    range.primitiveCount = triCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pranges = &range;

    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_xpu_vk.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (g_xpu_vk.vkAllocateCommandBuffers(g_xpu_vk.device, &cbai, &cmd)
            != VK_SUCCESS)
        goto tri_done;
    VkCommandBufferBeginInfo cbbi;
    memset(&cbbi, 0, sizeof(cbbi));
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_xpu_vk.vkBeginCommandBuffer(cmd, &cbbi);
    g_xpu_vk.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bgi, &pranges);
    g_xpu_vk.vkEndCommandBuffer(cmd);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (g_xpu_vk.vkQueueSubmit(g_xpu_vk.queue, 1, &si, VK_NULL_HANDLE)
            != VK_SUCCESS)
        goto tri_done;
    g_xpu_vk.vkQueueWaitIdle(g_xpu_vk.queue);

    // 7. Record the AS (asBuf/asMem/accel survive).
    {
        int slot = -1;
        for (int i = 0; i < g_vk_accel_count; ++i)
            if (!g_vk_accels[i].live) { slot = i; break; }
        if (slot < 0) {
            if (g_vk_accel_count >= CAJETA_VK_MAX_ACCELS) goto tri_done;
            slot = g_vk_accel_count++;
        }
        g_vk_accels[slot].accel = accel;
        g_vk_accels[slot].asBuf = asBuf;
        g_vk_accels[slot].asMem = asMem;
        g_vk_accels[slot].live = 1;
        result = (int64_t) (slot + 1);
        accel = VK_NULL_HANDLE; asBuf = VK_NULL_HANDLE; asMem = VK_NULL_HANDLE;
    }

tri_done:
    if (cmd) g_xpu_vk.vkFreeCommandBuffers(g_xpu_vk.device, g_xpu_vk.cmdPool, 1, &cmd);
    if (scratchBuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, scratchBuf, NULL);
    if (scratchMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, scratchMem, NULL);
    if (vMapped) g_xpu_vk.vkUnmapMemory(g_xpu_vk.device, vmem);
    if (vbuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, vbuf, NULL);
    if (vmem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, vmem, NULL);
    if (accel) g_xpu_vk.vkDestroyAccelerationStructureKHR(g_xpu_vk.device, accel, NULL);
    if (asBuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, asBuf, NULL);
    if (asMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, asMem, NULL);
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return result;
}

static void cajeta_xpu_vk_accel_free(int64_t handle) {
    pthread_mutex_lock(&g_xpu_vk_submit_mu);   // serialize vs build/launch + table
    struct cajeta_vk_accel* a = cajeta_xpu_vk_accel_rec(handle);
    if (!a) { pthread_mutex_unlock(&g_xpu_vk_submit_mu); return; }
    if (a->accel)
        g_xpu_vk.vkDestroyAccelerationStructureKHR(g_xpu_vk.device, a->accel, NULL);
    if (a->asBuf) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, a->asBuf, NULL);
    if (a->asMem) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, a->asMem, NULL);
    a->accel = VK_NULL_HANDLE; a->asBuf = VK_NULL_HANDLE;
    a->asMem = VK_NULL_HANDLE; a->live = 0;
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
}

// One dispatch: shader module + descriptor set (binding i = bindings[i]) +
// pipeline + command buffer + submit + wait. `bindings` are 1-based table
// handles, in kernel-parameter order. Mirrors VulkanDriver::launch.
// Per-binding resource kind, after launch_vulkan has wrapped scalars into SSBOs.
#define CAJ_VKB_BUFFER  0   // bindings[i] = buffer-table handle  -> STORAGE_BUFFER
#define CAJ_VKB_TEXTURE 1   // bindings[i] = texture-table handle -> SAMPLED_IMAGE
#define CAJ_VKB_SAMPLER 2   // bindings[i] = (int64) VkSampler    -> SAMPLER
#define CAJ_VKB_ACCEL   3   // bindings[i] = accel-table handle   -> ACCELERATION_STRUCTURE_KHR
#define CAJ_VKB_STORAGE_IMAGE 4 // bindings[i] = texture-table handle (storage) -> STORAGE_IMAGE
#define CAJ_VKB_BUFFER_ARRAY  5 // bindings[i] = ptr to [int64 count, int64 h0…] -> STORAGE_BUFFER array
// Fixed bindless descriptor-array size — MUST equal the SPIR-V handlefrombinding
// `range` (kMaxBindlessBuffers in SpirvKernelLowering.cpp) and the launch
// marshalling cap (CallExpression.cpp). The layout binds this many descriptors;
// the runtime fills `count` real + pads the rest with a valid buffer.
#define CAJ_VK_BINDLESS_MAX 16

static VkDescriptorType cajeta_vkb_desc_type(uint8_t kind) {
    if (kind == CAJ_VKB_TEXTURE) return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    if (kind == CAJ_VKB_STORAGE_IMAGE) return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    if (kind == CAJ_VKB_SAMPLER) return VK_DESCRIPTOR_TYPE_SAMPLER;
    if (kind == CAJ_VKB_ACCEL) return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
}

static int cajeta_xpu_vk_launch(const void* spirv, uint64_t len,
                                const char* entry, const int64_t* bindings,
                                const uint8_t* kinds, int n,
                                unsigned gx, unsigned gy, unsigned gz,
                                unsigned bx, unsigned by, unsigned bz,
                                unsigned sharedBytes) {
    if (!spirv || len < 4 || n <= 0) return 0;
    // Serialize the dispatch: VkQueue + VkCommandPool require external host
    // synchronization, and an AS binding reads the g_vk_accels table — all shared
    // with the build/free paths, which may run on a different OS thread.
    pthread_mutex_lock(&g_xpu_vk_submit_mu);
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    int ok = 0;

    VkShaderModuleCreateInfo smci;
    memset(&smci, 0, sizeof(smci));
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = (size_t) len;
    smci.pCode = (const uint32_t*) spirv;
    if (g_xpu_vk.vkCreateShaderModule(g_xpu_vk.device, &smci, NULL, &module)
            != VK_SUCCESS) goto done;

    VkDescriptorSetLayoutBinding binds[64];
    if (n > 64) goto done;
    memset(binds, 0, sizeof(binds[0]) * n);
    for (int i = 0; i < n; ++i) {
        binds[i].binding = (uint32_t) i;
        binds[i].descriptorType = cajeta_vkb_desc_type(kinds[i]);
        binds[i].descriptorCount =
            (kinds[i] == CAJ_VKB_BUFFER_ARRAY) ? CAJ_VK_BINDLESS_MAX : 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo slci;
    memset(&slci, 0, sizeof(slci));
    slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    slci.bindingCount = (uint32_t) n;
    slci.pBindings = binds;
    if (g_xpu_vk.vkCreateDescriptorSetLayout(g_xpu_vk.device, &slci, NULL,
                                             &setLayout) != VK_SUCCESS) goto done;

    VkPipelineLayoutCreateInfo plci;
    memset(&plci, 0, sizeof(plci));
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout;
    if (g_xpu_vk.vkCreatePipelineLayout(g_xpu_vk.device, &plci, NULL,
                                        &pipeLayout) != VK_SUCCESS) goto done;

    // Spec constants: SpecId 0/1/2 = block.x/y/z (workgroup size, see
    // injectWorkgroupSizeSpecConstant), SpecId 3 = the dynamic shared array's
    // length in elements (= sharedBytes / 4; the dynamic-shared element is 4 bytes
    // — int32/float32 — for now). Set at pipeline creation.
    uint32_t specData[4] = { bx, by, bz, sharedBytes / 4u };
    VkSpecializationMapEntry specEntries[4] = {
        { 0, 0,                    sizeof(uint32_t) },
        { 1, sizeof(uint32_t),     sizeof(uint32_t) },
        { 2, 2 * sizeof(uint32_t), sizeof(uint32_t) },
        { 3, 3 * sizeof(uint32_t), sizeof(uint32_t) },
    };
    VkSpecializationInfo specInfo;
    specInfo.mapEntryCount = 4;
    specInfo.pMapEntries = specEntries;
    specInfo.dataSize = sizeof(specData);
    specInfo.pData = specData;

    VkComputePipelineCreateInfo cpci;
    memset(&cpci, 0, sizeof(cpci));
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module;
    cpci.stage.pName = entry;
    cpci.stage.pSpecializationInfo = &specInfo;
    cpci.layout = pipeLayout;
    if (g_xpu_vk.vkCreateComputePipelines(g_xpu_vk.device, VK_NULL_HANDLE, 1,
                                          &cpci, NULL, &pipeline) != VK_SUCCESS)
        goto done;

    // Pool sized by the distinct descriptor types actually used (storage
    // buffer / sampled image / sampler), one entry per non-empty class.
    uint32_t nBuf = 0, nImg = 0, nStor = 0, nSamp = 0, nAccel = 0;
    for (int i = 0; i < n; ++i) {
        if (kinds[i] == CAJ_VKB_TEXTURE) ++nImg;
        else if (kinds[i] == CAJ_VKB_STORAGE_IMAGE) ++nStor;
        else if (kinds[i] == CAJ_VKB_SAMPLER) ++nSamp;
        else if (kinds[i] == CAJ_VKB_ACCEL) ++nAccel;
        else if (kinds[i] == CAJ_VKB_BUFFER_ARRAY) nBuf += CAJ_VK_BINDLESS_MAX;
        else ++nBuf;
    }
    VkDescriptorPoolSize poolSizes[5];
    uint32_t nPool = 0;
    if (nBuf) { poolSizes[nPool].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                poolSizes[nPool++].descriptorCount = nBuf; }
    if (nImg) { poolSizes[nPool].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                poolSizes[nPool++].descriptorCount = nImg; }
    if (nStor){ poolSizes[nPool].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                poolSizes[nPool++].descriptorCount = nStor; }
    if (nSamp){ poolSizes[nPool].type = VK_DESCRIPTOR_TYPE_SAMPLER;
                poolSizes[nPool++].descriptorCount = nSamp; }
    if (nAccel){ poolSizes[nPool].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                 poolSizes[nPool++].descriptorCount = nAccel; }
    VkDescriptorPoolCreateInfo dpci;
    memset(&dpci, 0, sizeof(dpci));
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = nPool;
    dpci.pPoolSizes = poolSizes;
    if (g_xpu_vk.vkCreateDescriptorPool(g_xpu_vk.device, &dpci, NULL, &descPool)
            != VK_SUCCESS) goto done;

    VkDescriptorSetAllocateInfo dsai;
    memset(&dsai, 0, sizeof(dsai));
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = descPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &setLayout;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    if (g_xpu_vk.vkAllocateDescriptorSets(g_xpu_vk.device, &dsai, &descSet)
            != VK_SUCCESS) goto done;

    VkDescriptorBufferInfo bufInfos[64];
    // Per-array-binding descriptor infos (CAJ_VK_BINDLESS_MAX each). One row per
    // binding index; only buffer-array bindings use their row.
    VkDescriptorBufferInfo arrInfos[64 * CAJ_VK_BINDLESS_MAX];
    VkDescriptorImageInfo imgInfos[64];
    VkWriteDescriptorSet writes[64];
    // Acceleration-structure writes chain their AS handle in via pNext; both the
    // pNext struct and the handle it points at must outlive vkUpdateDescriptorSets.
    VkWriteDescriptorSetAccelerationStructureKHR accelInfos[64];
    VkAccelerationStructureKHR accelHandles[64];
    memset(writes, 0, sizeof(writes[0]) * n);
    memset(imgInfos, 0, sizeof(imgInfos[0]) * n);
    memset(accelInfos, 0, sizeof(accelInfos[0]) * n);
    for (int i = 0; i < n; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descSet;
        writes[i].dstBinding = (uint32_t) i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = cajeta_vkb_desc_type(kinds[i]);
        if (kinds[i] == CAJ_VKB_ACCEL) {
            struct cajeta_vk_accel* a = cajeta_xpu_vk_accel_rec(bindings[i]);
            if (!a) goto done;
            accelHandles[i] = a->accel;
            accelInfos[i].sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            accelInfos[i].accelerationStructureCount = 1;
            accelInfos[i].pAccelerationStructures = &accelHandles[i];
            writes[i].pNext = &accelInfos[i];
        } else if (kinds[i] == CAJ_VKB_TEXTURE) {
            struct cajeta_vk_tex* t = cajeta_xpu_vk_tex_rec(bindings[i]);
            if (!t) goto done;
            imgInfos[i].imageView = t->view;
            imgInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[i].pImageInfo = &imgInfos[i];
        } else if (kinds[i] == CAJ_VKB_STORAGE_IMAGE) {
            // Writable storage image (Image2D): bound in GENERAL layout, the only
            // layout valid for an OpImageWrite descriptor. The pre-dispatch
            // barrier below transitions the image into GENERAL.
            struct cajeta_vk_tex* t = cajeta_xpu_vk_tex_rec(bindings[i]);
            if (!t) goto done;
            imgInfos[i].imageView = t->view;
            imgInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            writes[i].pImageInfo = &imgInfos[i];
        } else if (kinds[i] == CAJ_VKB_SAMPLER) {
            imgInfos[i].sampler = (VkSampler) (uintptr_t) bindings[i];
            if (imgInfos[i].sampler == VK_NULL_HANDLE) goto done;
            writes[i].pImageInfo = &imgInfos[i];
        } else if (kinds[i] == CAJ_VKB_BUFFER_ARRAY) {
            // bindings[i] points at the launch-marshalled [int64 count, int64
            // h0 … h(count-1)]. Bind CAJ_VK_BINDLESS_MAX descriptors: the first
            // `count` real, the rest padded with handle[0] (a valid buffer) so
            // every descriptor in the array is bound (avoids PARTIALLY_BOUND).
            // The kernel only reads bufs[0..count).
            const int64_t* arr = (const int64_t*) (intptr_t) bindings[i];
            int64_t cnt = arr ? arr[0] : 0;
            if (cnt < 1 || cnt > CAJ_VK_BINDLESS_MAX) goto done;
            VkDescriptorBufferInfo* row = &arrInfos[i * CAJ_VK_BINDLESS_MAX];
            for (int e = 0; e < CAJ_VK_BINDLESS_MAX; ++e) {
                int64_t h = arr[1 + (e < (int) cnt ? e : 0)];   // pad with elem 0
                struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(h);
                if (!r) goto done;
                row[e].buffer = r->buffer;
                row[e].offset = r->view_offset;
                row[e].range = VK_WHOLE_SIZE;
            }
            writes[i].descriptorCount = CAJ_VK_BINDLESS_MAX;
            writes[i].pBufferInfo = row;
        } else {
            struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(bindings[i]);
            if (!r) goto done;
            bufInfos[i].buffer = r->buffer;
            bufInfos[i].offset = r->view_offset;   // 0 for an owner; slice byte offset for a view
            bufInfos[i].range = VK_WHOLE_SIZE;
            writes[i].pBufferInfo = &bufInfos[i];
        }
    }
    g_xpu_vk.vkUpdateDescriptorSets(g_xpu_vk.device, (uint32_t) n, writes, 0,
                                    NULL);

    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_xpu_vk.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (g_xpu_vk.vkAllocateCommandBuffers(g_xpu_vk.device, &cbai, &cmd)
            != VK_SUCCESS) goto done;

    VkCommandBufferBeginInfo cbbi;
    memset(&cbbi, 0, sizeof(cbbi));
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_xpu_vk.vkBeginCommandBuffer(cmd, &cbbi);
    // Storage images (Image2D) must be in GENERAL layout for OpImageWrite /
    // OpImageRead. Barrier each before binding the pipeline. The barrier is
    // emitted even when the image is ALREADY GENERAL (a prior dispatch): then it
    // is not a layout transition but a read/write-after-write memory dependency,
    // so a kernel that loads what an earlier dispatch stored sees the new texels
    // (img.load reading a previous dispatch's img.store).
    for (int i = 0; i < n; ++i) {
        if (kinds[i] != CAJ_VKB_STORAGE_IMAGE) continue;
        struct cajeta_vk_tex* t = cajeta_xpu_vk_tex_rec(bindings[i]);
        if (!t) continue;
        int wasGeneral = (t->layout == VK_IMAGE_LAYOUT_GENERAL);
        VkImageMemoryBarrier toGen;
        memset(&toGen, 0, sizeof(toGen));
        toGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toGen.oldLayout = t->layout;
        toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGen.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGen.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGen.image = t->image;
        toGen.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toGen.subresourceRange.levelCount = 1;
        toGen.subresourceRange.layerCount = 1;
        // From GENERAL: a prior dispatch's shader writes must be made available
        // before this dispatch's shader read/write. From UNDEFINED/other: a plain
        // transition with no prior shader access to wait on.
        toGen.srcAccessMask = wasGeneral ? VK_ACCESS_SHADER_WRITE_BIT : 0;
        toGen.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        g_xpu_vk.vkCmdPipelineBarrier(
            cmd,
            wasGeneral ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                       : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
            0, NULL, 0, NULL, 1, &toGen);
        t->layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    g_xpu_vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    g_xpu_vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pipeLayout, 0, 1, &descSet, 0, NULL);
    g_xpu_vk.vkCmdDispatch(cmd, gx, gy, gz);   // 3-D grid (block dim is baked)
    g_xpu_vk.vkEndCommandBuffer(cmd);

    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (g_xpu_vk.vkQueueSubmit(g_xpu_vk.queue, 1, &si, VK_NULL_HANDLE)
            != VK_SUCCESS) goto done;
    if (g_xpu_vk.vkQueueWaitIdle(g_xpu_vk.queue) != VK_SUCCESS) goto done;
    ok = 1;

done:
    if (cmd) g_xpu_vk.vkFreeCommandBuffers(g_xpu_vk.device, g_xpu_vk.cmdPool, 1,
                                           &cmd);
    if (descPool) g_xpu_vk.vkDestroyDescriptorPool(g_xpu_vk.device, descPool,
                                                   NULL);
    if (pipeline) g_xpu_vk.vkDestroyPipeline(g_xpu_vk.device, pipeline, NULL);
    if (pipeLayout) g_xpu_vk.vkDestroyPipelineLayout(g_xpu_vk.device, pipeLayout,
                                                     NULL);
    if (setLayout) g_xpu_vk.vkDestroyDescriptorSetLayout(g_xpu_vk.device,
                                                         setLayout, NULL);
    if (module) g_xpu_vk.vkDestroyShaderModule(g_xpu_vk.device, module, NULL);
    pthread_mutex_unlock(&g_xpu_vk_submit_mu);
    return ok;
}

#else  // no Vulkan SDK header at runtime-build time — Vulkan unavailable.
static int cajeta_xpu_vulkan_init_locked(void) { return 0; }
static int64_t cajeta_xpu_vk_alloc(uint64_t b) { (void) b; return 0; }
static int64_t cajeta_xpu_vk_slice(int64_t p, uint64_t o) { (void) p; (void) o; return 0; }
static void* cajeta_xpu_vk_mapped(int64_t h) { (void) h; return NULL; }
static void cajeta_xpu_vk_free(int64_t h) { (void) h; }
static int64_t cajeta_xpu_vk_tex_alloc(uint32_t w, uint32_t h, int storage,
                                       int32_t format, uint32_t depth, int imageKind,
                                       uint32_t arrayLayers, uint32_t mipLevels) {
    (void) w; (void) h; (void) storage; (void) format; (void) depth; (void) imageKind;
    (void) arrayLayers; (void) mipLevels;
    return 0;
}
static void cajeta_xpu_vk_tex_upload(int64_t h, const float* src,
                                     uint32_t w, uint32_t ht, int32_t format) {
    (void) h; (void) src; (void) w; (void) ht; (void) format;
}
static void cajeta_xpu_vk_tex_upload_level(int64_t h, const float* src,
                                           uint32_t lw, uint32_t lh,
                                           uint32_t level, int32_t format) {
    (void) h; (void) src; (void) lw; (void) lh; (void) level; (void) format;
}
static void cajeta_xpu_vk_tex_download(int64_t h, void* d,
                                       uint32_t w, uint32_t ht) {
    (void) h; (void) d; (void) w; (void) ht;
}
static void cajeta_xpu_vk_tex_free(int64_t h) { (void) h; }
static int64_t cajeta_xpu_vk_make_sampler(int32_t f, int32_t a) {
    (void) f; (void) a; return 0;
}
static void cajeta_xpu_vk_destroy_sampler(int64_t h) { (void) h; }
static int64_t cajeta_xpu_vk_accel_build_aabbs(const float* a, uint32_t c) {
    (void) a; (void) c; return 0;
}
static int64_t cajeta_xpu_vk_accel_build_triangles(const float* v, uint32_t t,
                                                   uint32_t s) {
    (void) v; (void) t; (void) s; return 0;
}
static void cajeta_xpu_vk_accel_free(int64_t h) { (void) h; }
static int cajeta_xpu_vk_launch(const void* s, uint64_t l, const char* e,
                                const int64_t* b, const uint8_t* k, int n,
                                unsigned gx, unsigned gy, unsigned gz,
                                unsigned bx, unsigned by, unsigned bz,
                                unsigned sharedBytes) {
    (void) s; (void) l; (void) e; (void) b; (void) k; (void) n;
    (void) gx; (void) gy; (void) gz; (void) bx; (void) by; (void) bz;
    (void) sharedBytes; return 0;
}
#endif  // CAJETA_RT_HAS_VULKAN

// --- registered kernel modules (cubin images keyed by PTX entry name) -------
// The device-cubin pass emits a global constructor that calls
// __cajeta_xpu_register_module once per @Kernel; the launch path loads the
// CUDA module + resolves the function lazily on first use of each name.
struct cajeta_xpu_module {
    char name[256];
    const void* image;
    uint64_t len;     // image byte length (SPIR-V needs it; CUDA/HIP ignore it)
    void* module;     // CUmodule/hipModule, lazily loaded
    void* function;   // CUfunction/hipFunction, lazily resolved
};
#define CAJETA_XPU_MAX_MODULES 128
static struct cajeta_xpu_module g_xpu_modules[CAJETA_XPU_MAX_MODULES];
static int g_xpu_module_count;

// Caller holds g_xpu_cuda_lock.
static struct cajeta_xpu_module* cajeta_xpu_find_module(const char* name) {
    int i;
    for (i = 0; i < g_xpu_module_count; i++) {
        if (strncmp(g_xpu_modules[i].name, name,
                    sizeof(g_xpu_modules[i].name)) == 0) {
            return &g_xpu_modules[i];
        }
    }
    return NULL;
}

// --- Stream -----------------------------------------------------------------
// Streams. The Stream handle (int64) is the per-backend stream object: 0 = the
// default stream (the original v1 behaviour; current() returns it and the launch
// path passes NULL for it). create() makes a REAL stream (hipStreamCreate /
// cuStreamCreate) so async copies + stream-ordered launches queue independently;
// sync() drains either the named stream (real handle) or the whole context (0).
// Defined with the backend dispatcher below (after the kernel registries).
static void cajeta_xpu_sync_active(void);

int64_t __cajeta_xpu_stream_current(void) { return 0; }   // the default stream
// __cajeta_xpu_stream_{create,sync,destroy,wait_for} and the Event/Fence natives
// are defined further below, after the backend enum + cajeta_xpu_active_backend()
// they switch on (alongside the buffer async-copy functions).

// --- Thread / Workgroup coordinate readers ---------------------------------
// Returns zero in v1; step 7 plumbs these into TLS set by the emulation
// dispatch loop so kernel bodies see real thread indices.
uint32_t __cajeta_xpu_thread_x(void) { return 0; }
uint32_t __cajeta_xpu_thread_y(void) { return 0; }
uint32_t __cajeta_xpu_thread_z(void) { return 0; }
uint32_t __cajeta_xpu_thread_global_id_x(void) { return 0; }
uint32_t __cajeta_xpu_thread_global_id_y(void) { return 0; }
uint32_t __cajeta_xpu_thread_global_id_z(void) { return 0; }
uint32_t __cajeta_xpu_workgroup_x(void) { return 0; }
uint32_t __cajeta_xpu_workgroup_y(void) { return 0; }
uint32_t __cajeta_xpu_workgroup_z(void) { return 0; }
uint32_t __cajeta_xpu_workgroup_dim_x(void) { return 0; }
uint32_t __cajeta_xpu_workgroup_dim_y(void) { return 0; }
uint32_t __cajeta_xpu_workgroup_dim_z(void) { return 0; }

// --- Barrier ---------------------------------------------------------------
void __cajeta_xpu_barrier_workgroup(void) { /* no-op on CPU emulation */ }
void __cajeta_xpu_barrier_wave(void) { /* no-op on CPU emulation */ }

// --- Wave ------------------------------------------------------------------
// width=1 on CPU emulation (single-threaded) is the variance-correct
// default that doesn't make any kernel's wave-uniformity assumption
// false on this backend.
uint32_t __cajeta_xpu_wave_width(void) { return 1; }
// Lane within the wave: 0 on the width-1 emulation (only lane 0 exists). In a
// vectorized CPU kernel the lowering computes `tid.x % width` inline instead of
// calling this stub; this is the host @Native / scalar-fallback value.
uint32_t __cajeta_xpu_wave_lane_id(void) { return 0; }
// Width-1 emulation: the single lane is always the first.
bool __cajeta_xpu_wave_is_first_lane(void) { return true; }
uint32_t __cajeta_xpu_wave_shuffle_sync_u32(uint32_t value, uint32_t srcLane) {
    (void)srcLane; return value;
}
uint64_t __cajeta_xpu_wave_ballot_sync(bool predicate) {
    return predicate ? 1ULL : 0ULL;
}
// Single-lane wave (width=1) on CPU emulation: the wave-wide reduction of one
// lane's value is just that value. The real cross-lane reduction happens in the
// vectorized VFABI variant (CpuRegistration) when a wave kernel is widened;
// these scalars are the width-1 fallback.
uint32_t __cajeta_xpu_wave_reduce_sum_u32(uint32_t value) { return value; }
uint32_t __cajeta_xpu_wave_reduce_max_u32(uint32_t value) { return value; }
uint32_t __cajeta_xpu_wave_reduce_min_u32(uint32_t value) { return value; }
uint32_t __cajeta_xpu_wave_reduce_and_u32(uint32_t value) { return value; }
uint32_t __cajeta_xpu_wave_reduce_or_u32(uint32_t value) { return value; }
uint32_t __cajeta_xpu_wave_reduce_xor_u32(uint32_t value) { return value; }
// Exclusive prefix scan: width-1 fallback — lane 0's exclusive prefix is the
// identity (0 for sum, 1 for product). The real scan runs in the VFABI variant.
uint32_t __cajeta_xpu_wave_prefix_sum_u32(uint32_t value) { (void)value; return 0; }
uint32_t __cajeta_xpu_wave_prefix_product_u32(uint32_t value) { (void)value; return 1; }
// Width-1 wave rotate: a single-lane wave rotated by any delta is the lane
// itself (the host @Native / scalar fallback; the device path is ds_bpermute /
// OpGroupNonUniformRotateKHR). Matches the shuffle/reduce width-1 convention.
uint32_t __cajeta_xpu_wave_rotate_u32(uint32_t value, uint32_t delta) {
    (void)delta; return value;
}

// Quad (2x2) ops (Quad.*). Like the wave ops these are cross-lane on device; the
// host @Native definition is the width-1 (single-lane quad) fallback so the host
// JIT / any CPU @Device call resolves the Quad.cajeta forwarders. The device
// backends lower them inline (OpGroupNonUniformQuad* on Vulkan, ds_bpermute /
// shuffle elsewhere). A lone quad lane: broadcast/swap yield the lane's own
// value; the vote is just this lane's predicate.
uint32_t __cajeta_xpu_quad_broadcast(uint32_t value, uint32_t index) {
    (void)index; return value;
}
uint32_t __cajeta_xpu_quad_swap_horizontal(uint32_t value) { return value; }
uint32_t __cajeta_xpu_quad_swap_vertical(uint32_t value) { return value; }
uint32_t __cajeta_xpu_quad_swap_diagonal(uint32_t value) { return value; }
bool __cajeta_xpu_quad_all(bool predicate) { return predicate; }
bool __cajeta_xpu_quad_any(bool predicate) { return predicate; }

// Per-invocation bit ops (Bits.*). Unlike the wave ops these are NOT cross-lane —
// they are pure scalar functions of one u32, so the host @Native definition is
// the exact same computation the device emits (OpBitReverse / OpBitCount / a
// masked rotate). Provided so the host JIT (and any CPU @Device call) resolves
// the Bits.cajeta forwarders; the device backends lower them inline.
uint32_t __cajeta_xpu_bits_reverse_u32(uint32_t value) {
    value = ((value & 0x55555555u) << 1)  | ((value >> 1)  & 0x55555555u);
    value = ((value & 0x33333333u) << 2)  | ((value >> 2)  & 0x33333333u);
    value = ((value & 0x0F0F0F0Fu) << 4)  | ((value >> 4)  & 0x0F0F0F0Fu);
    value = ((value & 0x00FF00FFu) << 8)  | ((value >> 8)  & 0x00FF00FFu);
    value = (value << 16) | (value >> 16);
    return value;
}
uint32_t __cajeta_xpu_bits_count_u32(uint32_t value) {
    uint32_t c = 0;
    while (value) { value &= value - 1u; ++c; }
    return c;
}
uint32_t __cajeta_xpu_bits_rotate_left_u32(uint32_t value, uint32_t amount) {
    amount &= 31u;
    return amount == 0u ? value : ((value << amount) | (value >> (32u - amount)));
}
uint32_t __cajeta_xpu_bits_rotate_right_u32(uint32_t value, uint32_t amount) {
    amount &= 31u;
    return amount == 0u ? value : ((value >> amount) | (value << (32u - amount)));
}

// --- CPU backend kernel registry -------------------------------------------
// The CPU backend (cajeta-cpu.md) lowers each @Kernel to a host function linked
// into the program. Its registration ctor calls register_cpu_kernel(name, fn)
// at startup; the runtime dispatcher (Increment 4) resolves a launch to the
// stored pointer. Keyed by simple kernel name, matching the device backends'
// name-keyed __cajeta_xpu_register_module. A small fixed table — kernel counts
// are tiny — with last-writer-wins on a duplicate name.
#ifndef CAJETA_XPU_CPU_KERNEL_MAX
#define CAJETA_XPU_CPU_KERNEL_MAX 256
#endif
static struct { const char* name; void* fn; } g_cpu_kernels[CAJETA_XPU_CPU_KERNEL_MAX];
static int g_cpu_kernel_count = 0;

void __cajeta_xpu_register_cpu_kernel(const char* name, void* fn) {
    if (!name || !fn) return;
    for (int i = 0; i < g_cpu_kernel_count; ++i) {
        if (g_cpu_kernels[i].name && strcmp(g_cpu_kernels[i].name, name) == 0) {
            g_cpu_kernels[i].fn = fn;  // last writer wins
            return;
        }
    }
    if (g_cpu_kernel_count < CAJETA_XPU_CPU_KERNEL_MAX) {
        // Own the name: a caller may free the string after registering (notably
        // a JIT'd registration ctor whose module/engine is later torn down — the
        // kname global lives in JIT memory). Keeping the raw pointer leaves a
        // dangling key that the next strcmp() here or in lookup dereferences →
        // crash. strdup so the registry's keys outlive any caller (matching the
        // env-registry above). Process-lifetime table, never freed.
        g_cpu_kernels[g_cpu_kernel_count].name = strdup(name);
        g_cpu_kernels[g_cpu_kernel_count].fn = fn;
        ++g_cpu_kernel_count;
    }
}

// Resolve a registered CPU kernel by name (NULL if absent). Used by the
// dispatcher; exposed now so registration is testable end-to-end.
void* __cajeta_xpu_lookup_cpu_kernel(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < g_cpu_kernel_count; ++i) {
        if (g_cpu_kernels[i].name && strcmp(g_cpu_kernels[i].name, name) == 0) {
            return g_cpu_kernels[i].fn;
        }
    }
    return 0;
}

// --- Backend dispatcher (cajeta-cpu.md Increment 4) -------------------------
// Compiled Cajeta programs launch through THIS C runtime only (the C++
// CudaDriver/HipDriver/VulkanDriver/CpuDriver are compiler/test-only and never
// linked into a user program). A binary can bundle several backends
// (--xpu-backend=vulkan,cpu); at the first device touch we pick the
// highest-priority one that is both BUNDLED (a compile-time manifest of ctors
// calling __cajeta_xpu_register_backend) and AVAILABLE (a runtime probe),
// honoring a CAJETA_XPU_BACKEND force-override, then cache it. Every device
// entry point (buffer_*, launch) routes to that backend. The choice is made
// ONCE — a GPU present at startup but lost mid-run is a hard error, not a silent
// CPU re-run (locked decision #2).
//
// Priority order: CUDA -> HIP -> Vulkan -> CPU. CPU is always available, the
// guaranteed terminal of the chain. Backend ids are the priority order.
enum {
    CAJ_XPU_CUDA   = 0,
    CAJ_XPU_HIP    = 1,
    CAJ_XPU_VULKAN = 2,
    CAJ_XPU_CPU    = 3,
    CAJ_XPU_COUNT  = 4,
    CAJ_XPU_NONE   = -1
};

static unsigned g_xpu_bundled;        // bit i set iff backend i was bundled
static int g_xpu_active = -2;         // -2 unselected, -1 none, else a backend id

static const char* cajeta_xpu_backend_name(int id) {
    switch (id) {
        case CAJ_XPU_CUDA:   return "cuda";
        case CAJ_XPU_HIP:    return "hip";
        case CAJ_XPU_VULKAN: return "vulkan";
        case CAJ_XPU_CPU:    return "cpu";
        default:             return "?";
    }
}

static int cajeta_xpu_backend_id_by_name(const char* s) {
    if (!s) return CAJ_XPU_NONE;
    for (int id = 0; id < CAJ_XPU_COUNT; ++id)
        if (strcmp(s, cajeta_xpu_backend_name(id)) == 0) return id;
    return CAJ_XPU_NONE;
}

// The compiler emits one ctor per bundled backend (Compiler::emitXpuKernels).
void __cajeta_xpu_register_backend(int32_t id) {
    if (id < 0 || id >= CAJ_XPU_COUNT) return;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    g_xpu_bundled |= (1u << id);
    pthread_mutex_unlock(&g_xpu_cuda_lock);
}

// Probe a backend's availability. Caller holds g_xpu_cuda_lock — so this calls
// the *_init_locked variants directly (NOT the locking *_ready wrappers, which
// would deadlock under the held lock). Vulkan lands in Increment 4.3; until then
// it probes unavailable, so a vulkan-only bundle falls through to the precise
// diagnostic (or to CPU if bundled).
static int cajeta_xpu_backend_available_locked(int id) {
    switch (id) {
        case CAJ_XPU_CUDA:   return cajeta_xpu_cuda_init_locked();
        case CAJ_XPU_HIP:    return cajeta_xpu_hip_init_locked();
        case CAJ_XPU_VULKAN: return cajeta_xpu_vulkan_init_locked();
        case CAJ_XPU_CPU:    return 1;
        default:             return 0;
    }
}

// Caller holds g_xpu_cuda_lock. Picks + caches the active backend.
static int cajeta_xpu_select_locked(void) {
    if (g_xpu_active != -2) return g_xpu_active;
    int forced = cajeta_xpu_backend_id_by_name(getenv("CAJETA_XPU_BACKEND"));
    for (int id = 0; id < CAJ_XPU_COUNT; ++id) {
        if (forced != CAJ_XPU_NONE && id != forced) continue;
        if (!(g_xpu_bundled & (1u << id))) continue;     // not bundled in
        if (cajeta_xpu_backend_available_locked(id)) { g_xpu_active = id; return id; }
    }
    g_xpu_active = CAJ_XPU_NONE;
    // Precise, once: degradation is a build-time contract (locked decision #3).
    char set[128]; size_t n = 0; set[0] = '\0';
    for (int id = 0; id < CAJ_XPU_COUNT; ++id) {
        if (!(g_xpu_bundled & (1u << id))) continue;
        const char* nm = cajeta_xpu_backend_name(id);
        n += (size_t) snprintf(set + n, sizeof(set) - n, "%s%s",
                               set[0] ? ", " : "", nm);
        if (n >= sizeof(set)) break;
    }
    fprintf(stderr,
            "cajeta.xpu: no available backend among {%s}; rebuild with `cpu` "
            "to enable CPU fallback\n", set);
    return CAJ_XPU_NONE;
}

static int cajeta_xpu_active_backend(void) {
    int r;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    r = cajeta_xpu_select_locked();
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    return r;
}

// Device.supports(Capability) — does the active device advertise the capability
// natively? The capability heuristic's runtime input (cajeta.gpu.core.Device).
// `cap` is the Capability ordinal (the stable contract in Capability.cajeta).
// Returns 0/1. Append new capabilities as new cases; never renumber.
int32_t __cajeta_xpu_device_supports(int32_t cap) {
    int be = cajeta_xpu_active_backend();
    switch (cap) {
        case 0:  // RayQueryNative — hardware inline ray query
#if defined(CAJETA_RT_HAS_VULKAN) && !defined(_WIN32)
            return (be == CAJ_XPU_VULKAN && g_xpu_vk.rayQuery) ? 1 : 0;
#else
            (void) be; return 0;
#endif
        default: return 0;
    }
}

// Synchronize the active backend (called by stream.sync). active_backend() has
// already initialized the chosen backend, so its fn pointers are valid. CPU is
// synchronous (nothing to drain); none is a no-op.
static void cajeta_xpu_sync_active(void) {
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA: g_xpu_cuda.cuCtxSynchronize();   break;
        case CAJ_XPU_HIP:  g_xpu_hip.hipDeviceSynchronize(); break;
        default: break;
    }
}

// CPU launch: resolve the kernel's registered launcher thunk and run the
// grid->threads loop (the in-C twin of CpuDriver::launch; cajeta-cpu.md Inc 3),
// 1-D to match the host-source launch boundary. coord = [tid.xyz, ctaid.xyz,
// ntid.xyz]; argv is the kernelParams array shared across work-items.
typedef void (*cajeta_cpu_launch_fn)(void** argv, const int32_t* coord);

// One worker's slice of the grid: linear block indices [bStart, bEnd) of a
// gx*gy*gz block grid, each block sized (bx,by,bz) work-items.
struct cajeta_cpu_grid_slice {
    cajeta_cpu_launch_fn fn;
    void** argv;
    int32_t bx, by, bz;   // block (workgroup) dims → ntid.xyz
    int32_t gx, gy, gz;   // grid dims (in blocks) → for decoding ctaid.xyz
    int32_t bStart;       // linear block index range [bStart, bEnd)
    int32_t bEnd;
    int32_t dynShared;    // dynamic shared-memory byte count (coord[12])
};

// Run a contiguous slice of blocks. The launcher thunk is the per-BLOCK wrapper
// (Inc 5B): it loops the block's work-items internally (vectorized), so we call
// it ONCE PER BLOCK, setting ctaid.xyz + ntid.xyz + nctaid.xyz. coord =
// [tid.xyz (the wrapper's loop var), ctaid.xyz, ntid.xyz, nctaid.xyz, dynShared].
// nctaid (grid block-count = gx,gy,gz) lets the kernel compute the grid-stride
// for-each stride gridSize = nctaid·ntid (Item 6 Stage 2). Each worker owns its
// coord (no sharing); a data-parallel CPU kernel writes disjoint elements, so
// the fan-out is race-free for any kernel correct on a GPU. The 3-D grid is
// linearized (x fastest) and decoded back to ctaid.xyz per block.
static void cajeta_xpu_cpu_run_slice(const struct cajeta_cpu_grid_slice* s) {
    int32_t coord[13] = {0, 0, 0, 0, 0, 0, s->bx, s->by, s->bz,
                         s->gx, s->gy, s->gz, s->dynShared};
    int64_t gxy = (int64_t) s->gx * s->gy;   // M9: 64-bit — gx*gy can exceed i32,
                                             // wrapping to 0 -> divide-by-zero below
    for (int32_t lin = s->bStart; lin < s->bEnd; ++lin) {
        coord[3] = lin % s->gx;            // ctaid.x
        coord[4] = (lin / s->gx) % s->gy;  // ctaid.y
        coord[5] = (int32_t) (lin / gxy);  // ctaid.z
        s->fn(s->argv, coord);   // per-block; the wrapper loops work-items
    }
}

static void* cajeta_xpu_cpu_worker(void* arg) {
    cajeta_xpu_cpu_run_slice((const struct cajeta_cpu_grid_slice*) arg);
    return NULL;
}

// CPU launch (cajeta-cpu.md Inc 3 + Inc 5A). Resolve the kernel's launcher thunk
// and run the grid->threads loop, parallelized across cores: the gridX blocks
// are chunked across min(gridX, cores) worker threads (the calling thread runs
// the last slice while the others fan out). Below a work-item threshold — or
// with one core / one block — it runs serially, since thread fan-out costs more
// than a small launch saves. Workgroup barriers are safe here even though they
// make work-items rendezvous: fission (Inc 6) realizes a barrier *within* a
// single per-block wrapper call, and each wrapper call runs on one worker — so
// the grid of blocks stays embarrassingly parallel (work-items of a block never
// split across threads). True wave=SIMD-lane vectorization (Inc 5B) layers on
// top of each work-item call.
#ifndef CAJETA_XPU_CPU_PARALLEL_THRESHOLD
#define CAJETA_XPU_CPU_PARALLEL_THRESHOLD 4096   /* work-items */
#endif
#define CAJETA_XPU_CPU_MAX_WORKERS 256

static void cajeta_xpu_launch_cpu(const char* name,
                                  int32_t gridX, int32_t gridY, int32_t gridZ,
                                  int32_t blockX, int32_t blockY, int32_t blockZ,
                                  int32_t sharedBytes, void* argv) {
    void* p = __cajeta_xpu_lookup_cpu_kernel(name);
    if (!p) {
        fprintf(stderr, "cajeta.xpu: no registered CPU kernel '%s' to launch\n",
                name);
        return;
    }
    cajeta_cpu_launch_fn fn = (cajeta_cpu_launch_fn) p;
    if (gridX < 1) gridX = 1; if (gridY < 1) gridY = 1; if (gridZ < 1) gridZ = 1;
    // L6: sharedBytes becomes a per-block alloca on the worker's stack; an absurd
    // value (the launch's sharedBytes round-tripped through the spec constant)
    // would blow the stack. Bound it — real GPU shared memory is well under this.
    if (sharedBytes < 0 || (uint32_t) sharedBytes > (16u << 20)) {
        fprintf(stderr, "cajeta.xpu: CPU launch sharedBytes %d out of range "
                "(max 16 MiB); not launching '%s'\n", sharedBytes, name);
        return;
    }


    // CAJETA_XPU_CPU_SERIAL forces single-threaded execution — a deterministic
    // debug/oracle mode and the serial baseline for benchmarking. Read once.
    static int force_serial = -1;
    if (force_serial < 0) force_serial = getenv("CAJETA_XPU_CPU_SERIAL") ? 1 : 0;

    // Blocks fan out across threads (never the work-items of one block); the
    // 3-D grid is flattened to nblocks linear indices, decoded to ctaid.xyz in
    // run_slice.
    // M9: compute in 64-bit — gridX*gridY*gridZ in int32 wraps (negative ->
    // serial loop never runs, a silent no-op; or to 0 -> divide-by-zero in
    // run_slice). The CPU path indexes blocks with int32, so clamp an absurd grid
    // (>2^31 blocks runs serially anyway) with a diagnostic rather than wrap.
    int64_t nblocks64 = (int64_t) gridX * (int64_t) gridY * (int64_t) gridZ;
    if (nblocks64 > INT32_MAX) {
        fprintf(stderr, "cajeta.xpu: CPU grid block count %lld exceeds INT32_MAX; "
                "clamping to %d\n", (long long) nblocks64, INT32_MAX);
        nblocks64 = INT32_MAX;
    }
    int32_t nblocks = (int32_t) nblocks64;
    int64_t blockSize = (int64_t) (blockX > 0 ? blockX : 1) *
                        (int64_t) (blockY > 0 ? blockY : 1) *
                        (int64_t) (blockZ > 0 ? blockZ : 1);
    int64_t total = (int64_t) nblocks * blockSize;
#if defined(_WIN32)
    // sysconf/_SC_NPROCESSORS_ONLN is POSIX; on Windows ask the Win32 API
    // (windows.h is included at file scope above for the fiber/file-lock paths).
    SYSTEM_INFO cpu_si;
    GetSystemInfo(&cpu_si);
    long cores = (long) cpu_si.dwNumberOfProcessors;
#else
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (cores < 1) cores = 1;
    int32_t nworkers = (int32_t) ((long) nblocks < cores ? (long) nblocks : cores);
    if (nworkers > CAJETA_XPU_CPU_MAX_WORKERS) nworkers = CAJETA_XPU_CPU_MAX_WORKERS;

    // Serial path: forced, tiny launch, single core, or a single block.
    if (force_serial || nblocks <= 1 || nworkers <= 1 ||
        total < CAJETA_XPU_CPU_PARALLEL_THRESHOLD) {
        struct cajeta_cpu_grid_slice all = {fn, (void**) argv,
                                            blockX, blockY, blockZ,
                                            gridX, gridY, gridZ,
                                            0, nblocks, sharedBytes};
        cajeta_xpu_cpu_run_slice(&all);
        return;
    }

    // Parallel fan-out: chunk the nblocks linear block indices across `nworkers`.
    pthread_t threads[CAJETA_XPU_CPU_MAX_WORKERS];
    struct cajeta_cpu_grid_slice slices[CAJETA_XPU_CPU_MAX_WORKERS];
    char spawned[CAJETA_XPU_CPU_MAX_WORKERS];
    int32_t base = nblocks / nworkers, rem = nblocks % nworkers, cx = 0;
    for (int32_t i = 0; i < nworkers; ++i) {
        int32_t count = base + (i < rem ? 1 : 0);
        slices[i].fn = fn;
        slices[i].argv = (void**) argv;
        slices[i].bx = blockX; slices[i].by = blockY; slices[i].bz = blockZ;
        slices[i].gx = gridX;  slices[i].gy = gridY;  slices[i].gz = gridZ;
        slices[i].bStart = cx;
        slices[i].bEnd = cx + count;
        slices[i].dynShared = sharedBytes;
        cx += count;
        if (i == nworkers - 1) {
            spawned[i] = 0;   // the calling thread runs the last slice
        } else if (pthread_create(&threads[i], NULL, cajeta_xpu_cpu_worker,
                                  &slices[i]) == 0) {
            spawned[i] = 1;
        } else {
            spawned[i] = 0;   // spawn failed — run this slice inline, drop nothing
            cajeta_xpu_cpu_run_slice(&slices[i]);
        }
    }
    cajeta_xpu_cpu_run_slice(&slices[nworkers - 1]);
    for (int32_t i = 0; i < nworkers; ++i) {
        if (spawned[i]) pthread_join(threads[i], NULL);
    }
}

// --- Buffer<T> device memory (backend-dispatched) ---------------------------
// The Buffer<T> stdlib methods (alloc/upload/download/free) are ordinary
// Cajeta now; they construct the handle via `heap`/`stack` + the generated
// constructor and forward byte-sized primitives here. The element byte size
// is supplied by the compiler (Buffer<T>.elementBytes() intrinsic), so these
// symbols are monomorphism-independent: they speak only int64 handles and
// byte counts. The int64 handle is the active backend's device pointer (CUDA/
// HIP), buffer-table index (Vulkan), or host block (CPU) — consistent within a
// run because the backend is fixed at first device touch.
//
// `host` is a Cajeta T[] header — { i64 count, [count x T] data } laid out
// contiguously — so the element bytes begin at offset 8 (matches
// __cajeta_new_array_header). byteCount is count * sizeof(T), already
// computed caller-side.
// `self` is the Buffer instance pointer the instance-method forwarder passes;
// the device side is keyed on the int64 handle, so self is ignored.
// Buffer MemoryKind ordinals — the stable native contract; MUST match
// runtime/src/cajeta/xpu/core/MemoryKind.cajeta. Device = device-local memory
// with explicit upload/download (the default, original behaviour); Pinned =
// page-locked, device-accessible host memory; Unified = managed memory one
// pointer host AND device see (zero-copy on an integrated GPU).
enum {
    CAJ_MEMKIND_DEVICE  = 0,
    CAJ_MEMKIND_PINNED  = 1,
    CAJ_MEMKIND_UNIFIED = 2
};
int64_t __cajeta_xpu_buffer_alloc(void* self, uint64_t byteCount, int32_t kind) {
    (void) self;
    if (byteCount == 0) return 0;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA: {
            cajeta_cudeviceptr p = 0;
            if (kind == CAJ_MEMKIND_UNIFIED && g_xpu_cuda.cuMemAllocManaged) {
                // CU_MEM_ATTACH_GLOBAL = 1
                if (g_xpu_cuda.cuMemAllocManaged(&p, (size_t) byteCount, 1) != 0) return 0;
                return (int64_t) p;
            }
            if (kind == CAJ_MEMKIND_PINNED && g_xpu_cuda.cuMemHostAlloc) {
                void* hp = NULL;
                // CU_MEMHOSTALLOC_DEVICEMAP = 2 → device-accessible
                if (g_xpu_cuda.cuMemHostAlloc(&hp, (size_t) byteCount, 2) != 0) return 0;
                return (int64_t) (intptr_t) hp;
            }
            if (g_xpu_cuda.cuMemAlloc(&p, (size_t) byteCount) != 0) return 0;
            return (int64_t) p;
        }
        case CAJ_XPU_HIP: {
            void* p = NULL;
            if (kind == CAJ_MEMKIND_UNIFIED && g_xpu_hip.hipMallocManaged) {
                // hipMemAttachGlobal = 1
                if (g_xpu_hip.hipMallocManaged(&p, (size_t) byteCount, 1) != 0) return 0;
                return (int64_t) (intptr_t) p;
            }
            if (kind == CAJ_MEMKIND_PINNED && g_xpu_hip.hipHostMalloc) {
                // hipHostMallocMapped = 0x2 → device-accessible
                if (g_xpu_hip.hipHostMalloc(&p, (size_t) byteCount, 0x2) != 0) return 0;
                return (int64_t) (intptr_t) p;
            }
            if (g_xpu_hip.hipMalloc(&p, (size_t) byteCount) != 0) return 0;
            return (int64_t) (intptr_t) p;
        }
        case CAJ_XPU_VULKAN:
            // Vulkan buffers are already host-visible + coherent on this device
            // (effectively unified); kind needs no distinct path. handle =
            // buffer-table index.
            (void) kind;
            return cajeta_xpu_vk_alloc(byteCount);
        case CAJ_XPU_CPU: {
            // CPU "device" memory = host; every kind is host-accessible already.
            void* p = malloc((size_t) byteCount);
            return (int64_t) (intptr_t) p;
        }
        default: return 0;   // none: diagnostic emitted
    }
}
// Direct host access to a host-accessible buffer (Pinned/Unified, or CPU/Vulkan
// mapped) with NO device-transfer API — a plain memcpy in the shared address
// space (zero-copy: no PCIe copy / no managed migration on a discrete GPU, a
// host memcpy on an APU). dir != 0 stores host[]→buffer; dir == 0 loads
// buffer→host[]. A plain Device buffer on a discrete GPU has no host mapping, so
// this no-ops (use upload/download there). `host` is a cajeta array (8-byte
// header skipped); kind selects whether the HIP/CUDA handle is host-accessible.
void __cajeta_xpu_buffer_host_copy(void* self, int64_t handle, void* host,
                                   uint64_t byteCount, int32_t dir, int32_t kind) {
    (void) self;
    if (!handle || !host || byteCount == 0) return;
    void* hostArr = (void*) ((char*) host + 8);   // skip cajeta array header
    void* hp = NULL;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU:
            hp = (void*) (intptr_t) handle;
            break;
        case CAJ_XPU_HIP:
        case CAJ_XPU_CUDA:
            // managed (Unified) and pinned host handles are host-accessible
            // pointers; plain Device memory is not.
            if (kind == CAJ_MEMKIND_UNIFIED || kind == CAJ_MEMKIND_PINNED)
                hp = (void*) (intptr_t) handle;
            break;
        case CAJ_XPU_VULKAN:
            hp = cajeta_xpu_vk_mapped(handle);   // host-coherent mapping
            break;
        default:
            break;
    }
    if (!hp) return;   // not host-accessible (Device on a discrete GPU)
    if (dir) memcpy(hp, hostArr, (size_t) byteCount);
    else     memcpy(hostArr, hp, (size_t) byteCount);
}
// Async host↔device copies on a stream (Buffer.uploadAsync/downloadAsync). The
// copy is enqueued on `stream` (a Stream handle; 0 = the default stream) and
// completes by the next sync of that stream — so it overlaps other work queued
// elsewhere. CUDA/HIP issue the real async memcpy (best paired with pinned/
// unified host memory); CPU and the Vulkan host-coherent map copy synchronously
// (no async path, but semantically correct — done by the time sync returns).
void __cajeta_xpu_buffer_upload_async(void* self, int64_t handle, void* host,
                                      uint64_t byteCount, int64_t stream) {
    (void) self;
    if (!handle || !host || byteCount == 0) return;
    const void* data = (const void*) ((const char*) host + 8);
    void* st = (void*) (intptr_t) stream;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            if (g_xpu_cuda.cuMemcpyHtoDAsync)
                g_xpu_cuda.cuMemcpyHtoDAsync((cajeta_cudeviceptr) handle, data,
                                             (size_t) byteCount, st);
            else
                g_xpu_cuda.cuMemcpyHtoD((cajeta_cudeviceptr) handle, data,
                                        (size_t) byteCount);
            return;
        case CAJ_XPU_HIP:
            if (g_xpu_hip.hipMemcpyHtoDAsync)
                g_xpu_hip.hipMemcpyHtoDAsync((void*) (intptr_t) handle, data,
                                             (size_t) byteCount, st);
            else
                g_xpu_hip.hipMemcpyHtoD((void*) (intptr_t) handle, data,
                                        (size_t) byteCount);
            return;
        case CAJ_XPU_VULKAN: {
            void* m = cajeta_xpu_vk_mapped(handle);   // coherent map: immediate
            if (m) memcpy(m, data, (size_t) byteCount);
            return;
        }
        case CAJ_XPU_CPU:
            memcpy((void*) (intptr_t) handle, data, (size_t) byteCount);
            return;
        default: return;
    }
}
void __cajeta_xpu_buffer_download_async(void* self, int64_t handle, void* host,
                                        uint64_t byteCount, int64_t stream) {
    (void) self;
    if (!handle || !host || byteCount == 0) return;
    void* data = (void*) ((char*) host + 8);
    void* st = (void*) (intptr_t) stream;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            if (g_xpu_cuda.cuMemcpyDtoHAsync)
                g_xpu_cuda.cuMemcpyDtoHAsync(data, (cajeta_cudeviceptr) handle,
                                             (size_t) byteCount, st);
            else
                g_xpu_cuda.cuMemcpyDtoH(data, (cajeta_cudeviceptr) handle,
                                        (size_t) byteCount);
            return;
        case CAJ_XPU_HIP:
            if (g_xpu_hip.hipMemcpyDtoHAsync)
                g_xpu_hip.hipMemcpyDtoHAsync(data, (void*) (intptr_t) handle,
                                             (size_t) byteCount, st);
            else
                g_xpu_hip.hipMemcpyDtoH(data, (void*) (intptr_t) handle,
                                        (size_t) byteCount);
            return;
        case CAJ_XPU_VULKAN: {
            void* m = cajeta_xpu_vk_mapped(handle);
            if (m) memcpy(data, m, (size_t) byteCount);
            return;
        }
        case CAJ_XPU_CPU:
            memcpy(data, (const void*) (intptr_t) handle, (size_t) byteCount);
            return;
        default: return;
    }
}
// Stream create/sync/destroy — defined here (not with stream_current above)
// because they switch on the backend enum + cajeta_xpu_active_backend(), which
// are declared further down. Handle 0 = the default stream (the v1 behaviour).
int64_t __cajeta_xpu_stream_create(void) {
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA: {
            void* s = NULL;
            if (g_xpu_cuda.cuStreamCreate &&
                g_xpu_cuda.cuStreamCreate(&s, 0) == 0)
                return (int64_t) (intptr_t) s;
            return 0;   // no driver entry → fall back to the default stream
        }
        case CAJ_XPU_HIP: {
            void* s = NULL;
            if (g_xpu_hip.hipStreamCreate &&
                g_xpu_hip.hipStreamCreate(&s) == 0)
                return (int64_t) (intptr_t) s;
            return 0;
        }
        default: return 0;   // CPU/Vulkan: synchronous; the default stream
    }
}
void __cajeta_xpu_stream_sync(void* self, int64_t handle) {
    (void) self;
    void* st = (void*) (intptr_t) handle;
    if (st) {
        // Drain just this stream (its async copies + launches).
        switch (cajeta_xpu_active_backend()) {
            case CAJ_XPU_CUDA:
                if (g_xpu_cuda.cuStreamSynchronize) {
                    g_xpu_cuda.cuStreamSynchronize(st); return;
                }
                break;
            case CAJ_XPU_HIP:
                if (g_xpu_hip.hipStreamSynchronize) {
                    g_xpu_hip.hipStreamSynchronize(st); return;
                }
                break;
            default: break;
        }
    }
    cajeta_xpu_sync_active();   // default stream (0) or no per-stream entry
}
void __cajeta_xpu_stream_destroy(void* self, int64_t handle) {
    (void) self;
    void* st = (void*) (intptr_t) handle;
    if (!st) return;   // the default stream is not destroyed
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            if (g_xpu_cuda.cuStreamDestroy) g_xpu_cuda.cuStreamDestroy(st);
            return;
        case CAJ_XPU_HIP:
            if (g_xpu_hip.hipStreamDestroy) g_xpu_hip.hipStreamDestroy(st);
            return;
        default: return;
    }
}

// --- Event -----------------------------------------------------------------
// Cross-stream + host synchronisation. The handle (int64) IS the backend event
// object; create() returns it (0 = unavailable). On CUDA/HIP these wrap a real
// cuEvent/hipEvent so a second stream can wait on a first stream's recorded
// point device-side; on CPU/Vulkan work is synchronous, so an event is a
// sentinel (handle 1) that is always already-signaled (record/wait no-op, query
// true). Event and Fence share the backend mechanism — Event is the device-
// facing surface (Stream.waitFor), Fence the host-facing one.
int64_t __cajeta_xpu_event_create(void) {
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA: {
            void* e = NULL;
            if (g_xpu_cuda.cuEventCreate &&
                g_xpu_cuda.cuEventCreate(&e, 0) == 0)
                return (int64_t) (intptr_t) e;
            return 0;
        }
        case CAJ_XPU_HIP: {
            void* e = NULL;
            if (g_xpu_hip.hipEventCreate &&
                g_xpu_hip.hipEventCreate(&e) == 0)
                return (int64_t) (intptr_t) e;
            return 0;
        }
        default: return 1;   // CPU/Vulkan: synchronous, always-signaled sentinel
    }
}
void __cajeta_xpu_event_record(void* self, int64_t handle, int64_t streamHandle) {
    (void) self;
    void* e = (void*) (intptr_t) handle;
    void* st = (void*) (intptr_t) streamHandle;   // 0 = default stream
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            if (e && g_xpu_cuda.cuEventRecord) g_xpu_cuda.cuEventRecord(e, st);
            return;
        case CAJ_XPU_HIP:
            if (e && g_xpu_hip.hipEventRecord) g_xpu_hip.hipEventRecord(e, st);
            return;
        default: return;   // CPU/Vulkan: nothing to record (synchronous)
    }
}
void __cajeta_xpu_event_wait(void* self, int64_t handle) {
    (void) self;
    void* e = (void*) (intptr_t) handle;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            if (e && g_xpu_cuda.cuEventSynchronize)
                g_xpu_cuda.cuEventSynchronize(e);
            return;
        case CAJ_XPU_HIP:
            if (e && g_xpu_hip.hipEventSynchronize)
                g_xpu_hip.hipEventSynchronize(e);
            return;
        default: return;   // CPU/Vulkan: work already done
    }
}
bool __cajeta_xpu_event_query(void* self, int64_t handle) {
    (void) self;
    void* e = (void*) (intptr_t) handle;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            if (e && g_xpu_cuda.cuEventQuery)
                return g_xpu_cuda.cuEventQuery(e) == 0;
            return true;
        case CAJ_XPU_HIP:
            if (e && g_xpu_hip.hipEventQuery)
                return g_xpu_hip.hipEventQuery(e) == 0;
            return true;
        default: return true;   // CPU/Vulkan: always complete
    }
}
void __cajeta_xpu_event_destroy(void* self, int64_t handle) {
    (void) self;
    void* e = (void*) (intptr_t) handle;
    if (!e) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            if (g_xpu_cuda.cuEventDestroy) g_xpu_cuda.cuEventDestroy(e);
            return;
        case CAJ_XPU_HIP:
            if (g_xpu_hip.hipEventDestroy) g_xpu_hip.hipEventDestroy(e);
            return;
        default: return;
    }
}

// Stream.waitFor(event): insert a device-side wait on `event` into `stream`, so
// future launches on `stream` start only after `event` is signaled on its source
// stream. Synchronous backends (CPU/Vulkan) need no wait — ordering already holds.
void __cajeta_xpu_stream_wait_for(void* self, int64_t streamHandle,
                                  int64_t eventHandle) {
    (void) self;
    void* st = (void*) (intptr_t) streamHandle;   // 0 = default stream
    void* e = (void*) (intptr_t) eventHandle;
    if (!e) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            if (g_xpu_cuda.cuStreamWaitEvent)
                g_xpu_cuda.cuStreamWaitEvent(st, e, 0);
            return;
        case CAJ_XPU_HIP:
            if (g_xpu_hip.hipStreamWaitEvent)
                g_xpu_hip.hipStreamWaitEvent(st, e, 0);
            return;
        default: return;
    }
}

// --- Fence -----------------------------------------------------------------
// Host-observable signal. v1 backs Fence with the same backend event object as
// Event (an event IS host-waitable via cuEvent/hipEventSynchronize/Query):
// signal(stream) records the event at the stream's tail; waitHost()/query()
// block/poll the host on it. On CPU/Vulkan the synchronous sentinel applies.
int64_t __cajeta_xpu_fence_create(void) { return __cajeta_xpu_event_create(); }
void __cajeta_xpu_fence_signal(void* self, int64_t handle, int64_t streamHandle) {
    __cajeta_xpu_event_record(self, handle, streamHandle);
}
void __cajeta_xpu_fence_wait(void* self, int64_t handle) {
    __cajeta_xpu_event_wait(self, handle);
}
bool __cajeta_xpu_fence_query(void* self, int64_t handle) {
    return __cajeta_xpu_event_query(self, handle);
}
void __cajeta_xpu_fence_destroy(void* self, int64_t handle) {
    __cajeta_xpu_event_destroy(self, handle);
}
void __cajeta_xpu_buffer_upload(void* self, int64_t handle, void* host,
                                uint64_t byteCount) {
    (void) self;
    if (!handle || !host || byteCount == 0) return;
    const void* data = (const void*) ((const char*) host + 8);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            g_xpu_cuda.cuMemcpyHtoD((cajeta_cudeviceptr) handle, data,
                                    (size_t) byteCount);
            return;
        case CAJ_XPU_HIP:
            g_xpu_hip.hipMemcpyHtoD((void*) (intptr_t) handle, data,
                                    (size_t) byteCount);
            return;
        case CAJ_XPU_VULKAN: {
            void* m = cajeta_xpu_vk_mapped(handle);   // host-coherent mapping
            if (m) memcpy(m, data, (size_t) byteCount);
            return;
        }
        case CAJ_XPU_CPU:
            memcpy((void*) (intptr_t) handle, data, (size_t) byteCount);
            return;
        default: return;
    }
}
void __cajeta_xpu_buffer_download(void* self, int64_t handle, void* host,
                                  uint64_t byteCount) {
    (void) self;
    if (!handle || !host || byteCount == 0) return;
    void* data = (void*) ((char*) host + 8);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            g_xpu_cuda.cuMemcpyDtoH(data, (cajeta_cudeviceptr) handle,
                                    (size_t) byteCount);
            return;
        case CAJ_XPU_HIP:
            g_xpu_hip.hipMemcpyDtoH(data, (void*) (intptr_t) handle,
                                    (size_t) byteCount);
            return;
        case CAJ_XPU_VULKAN: {
            void* m = cajeta_xpu_vk_mapped(handle);
            if (m) memcpy(data, m, (size_t) byteCount);
            return;
        }
        case CAJ_XPU_CPU:
            memcpy(data, (const void*) (intptr_t) handle, (size_t) byteCount);
            return;
        default: return;
    }
}
void __cajeta_xpu_buffer_free(void* self, int64_t handle, int32_t kind) {
    (void) self;
    if (!handle) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            // Pinned host memory frees with cuMemFreeHost; device + managed
            // (Unified) free with cuMemFree.
            if (kind == CAJ_MEMKIND_PINNED && g_xpu_cuda.cuMemFreeHost)
                g_xpu_cuda.cuMemFreeHost((void*) (intptr_t) handle);
            else
                g_xpu_cuda.cuMemFree((cajeta_cudeviceptr) handle);
            return;
        case CAJ_XPU_HIP:
            // Pinned host memory frees with hipHostFree; device + managed
            // (Unified) free with hipFree.
            if (kind == CAJ_MEMKIND_PINNED && g_xpu_hip.hipHostFree)
                g_xpu_hip.hipHostFree((void*) (intptr_t) handle);
            else
                g_xpu_hip.hipFree((void*) (intptr_t) handle);
            return;
        case CAJ_XPU_VULKAN: (void) kind; cajeta_xpu_vk_free(handle); return;
        case CAJ_XPU_CPU:    (void) kind; free((void*) (intptr_t) handle); return;
        default: return;
    }
}
// Buffer.slice: resolve a sub-range base from a parent handle + byte offset.
// Pointer backends (CUDA/HIP/CPU) fold the offset into the device pointer; the
// returned handle indexes the slice's first element exactly like a base buffer,
// so the launch-arg and upload/download paths need no offset-awareness. Vulkan
// (handle = buffer-table index) allocates a borrowing view slot that carries
// the descriptor offset. The returned handle is non-owning on every backend —
// Buffer.owned is false for a view, so its drop never frees this.
int64_t __cajeta_xpu_buffer_slice(void* self, int64_t handle, uint64_t byteOffset) {
    (void) self;
    if (!handle) return 0;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
        case CAJ_XPU_HIP:
        case CAJ_XPU_CPU:
            return handle + (int64_t) byteOffset;   // pointer + byte offset
        case CAJ_XPU_VULKAN:
            return cajeta_xpu_vk_slice(handle, byteOffset);
        default: return 0;
    }
}

// --- HIP texture helpers (Item 8 Stage C) -----------------------------------
// On AMD a Texture2D is a hipArray (created here) whose handle is a pointer to a
// cajeta_hip_tex record. The hipTextureObject — which carries the image AND
// sampler SRDs the kernel's __ockl_image_sample_2D reads — is built per launch
// (cajeta_xpu_launch_hip) from this array + the paired Sampler's modes.
static int cajeta_hip_tex_supported(void) {
    return g_xpu_hip.hipMallocArray && g_xpu_hip.hipMemcpy2DToArray &&
           g_xpu_hip.hipCreateTextureObject && g_xpu_hip.hipDestroyTextureObject;
}

static int64_t cajeta_xpu_hip_tex_alloc(uint32_t w, uint32_t h, int32_t format) {
    if (!cajeta_hip_tex_supported()) return 0;
    int channels = cajeta_texfmt_channels(format);
    int bits = cajeta_texfmt_is_unorm(format) ? 8                 // per-channel
             : cajeta_texfmt_is_half(format)  ? 16
                                              : 32;
    struct caj_hip_channel_format_desc cd;
    memset(&cd, 0, sizeof(cd));
    cd.x = bits;
    if (channels == 4) { cd.y = bits; cd.z = bits; cd.w = bits; }
    // UNORM stores unsigned bytes (read back normalized to [0,1] via the texobj's
    // NormalizedFloat read mode); float/half store raw floats (16- or 32-bit) read
    // element-typed (Float channel kind); raw 32-bit integer formats store
    // signed/unsigned ints, read element-typed (the texobj readMode below is
    // Element, so image_load returns the raw integer bits — bitcast on the device).
    cd.f = cajeta_texfmt_is_integer(format)
               ? (cajeta_texfmt_is_unsigned(format) ? CAJ_HIP_CHANNEL_UNSIGNED
                                                     : CAJ_HIP_CHANNEL_SIGNED)
         : cajeta_texfmt_is_unorm(format) ? CAJ_HIP_CHANNEL_UNSIGNED
                                          : CAJ_HIP_CHANNEL_FLOAT;
    void* array = NULL;
    if (g_xpu_hip.hipMallocArray(&array, &cd, w, h, 0) != 0 || !array) return 0;
    struct cajeta_hip_tex* t =
        (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) { if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(array); return 0; }
    t->array = array; t->mipmap = NULL; t->w = w; t->h = h; t->d = 1;
    t->format = format; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

// --- Image2D storage images on AMD (the writable twin of the texture path) ---
// A storage image is a hipArray allocated with hipArraySurfaceLoadStore, bound
// per launch as a SURFACE object (no sampler). Optional, exactly like the
// texture path: when the symbols (or the driver) are absent the alloc returns 0
// and the feature degrades to unsupported (cf. the mipmap path on this APU).
static int cajeta_hip_surf_supported(void) {
    return g_xpu_hip.hipMallocArray && g_xpu_hip.hipFreeArray &&
           g_xpu_hip.hipCreateSurfaceObject && g_xpu_hip.hipDestroySurfaceObject &&
           g_xpu_hip.hipMemcpy2DFromArray;
}

// Allocate an R32F surface-capable hipArray (Image2D is R32F only, matching the
// Vulkan storage image). Reuses the cajeta_hip_tex record (format R32F, 1 level).
static int64_t cajeta_xpu_hip_image_alloc(uint32_t w, uint32_t h) {
    if (!cajeta_hip_surf_supported()) return 0;
    struct caj_hip_channel_format_desc cd;
    memset(&cd, 0, sizeof(cd));
    cd.x = 32;                       // single 32-bit channel
    cd.f = CAJ_HIP_CHANNEL_FLOAT;
    void* array = NULL;
    if (g_xpu_hip.hipMallocArray(&array, &cd, w, h,
                                 CAJ_HIP_ARRAY_SURFACE_LOAD_STORE) != 0 || !array)
        return 0;
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) { if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(array); return 0; }
    t->array = array; t->mipmap = NULL; t->w = w; t->h = h; t->d = 1;
    t->format = CAJ_TEXFMT_R32F; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

// Read the surface array back to host (the texels the kernel wrote). Row pitch
// and copy width are in BYTES (w * sizeof(float)); height is in rows.
static void cajeta_xpu_hip_image_download(int64_t handle, void* host,
                                          uint32_t w, uint32_t h) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) handle;
    if (!t || !t->array || !host || !g_xpu_hip.hipMemcpy2DFromArray) return;
    size_t rowBytes = (size_t) w * sizeof(float);
    g_xpu_hip.hipMemcpy2DFromArray(host, rowBytes, t->array, 0, 0, rowBytes, h,
                                   CAJ_HIP_MEMCPY_DTOH);
}

static void cajeta_xpu_hip_image_free(int64_t handle) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) handle;
    if (!t) return;
    if (t->array && g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(t->array);
    free(t);
}

// Build the channel-format descriptor for a TextureFormat (shared by 2-D + 3-D).
static struct caj_hip_channel_format_desc cajeta_hip_channel_desc(int32_t format) {
    int channels = cajeta_texfmt_channels(format);
    int bits = cajeta_texfmt_is_unorm(format) ? 8
             : cajeta_texfmt_is_half(format)  ? 16 : 32;
    struct caj_hip_channel_format_desc cd;
    memset(&cd, 0, sizeof(cd));
    cd.x = bits;
    if (channels == 4) { cd.y = bits; cd.z = bits; cd.w = bits; }
    cd.f = cajeta_texfmt_is_integer(format)
               ? (cajeta_texfmt_is_unsigned(format) ? CAJ_HIP_CHANNEL_UNSIGNED
                                                     : CAJ_HIP_CHANNEL_SIGNED)
         : cajeta_texfmt_is_unorm(format) ? CAJ_HIP_CHANNEL_UNSIGNED
                                          : CAJ_HIP_CHANNEL_FLOAT;
    return cd;
}

// Texture1D on AMD: a 1-D hipArray. hipMallocArray with height 0 makes a 1-D
// array, which yields a 1-D image SRD through the same dimension-agnostic
// RES_ARRAY texobj path — so the kernel's __ockl_image_{sample,load}_1D address
// it correctly. Upload + free reuse the 2-D paths (a 1-D array is a height-1
// hipMemcpy2DToArray).
static int64_t cajeta_xpu_hip_tex1d_alloc(uint32_t w, int32_t format) {
    if (!cajeta_hip_tex_supported()) return 0;
    struct caj_hip_channel_format_desc cd = cajeta_hip_channel_desc(format);
    void* array = NULL;
    if (g_xpu_hip.hipMallocArray(&array, &cd, w, 0, 0) != 0 || !array) return 0;
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) { if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(array); return 0; }
    t->array = array; t->mipmap = NULL; t->w = w; t->h = 1; t->d = 1;
    t->format = format; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

// Texture3D on AMD: a 3-D hipArray (hipMalloc3DArray) + per-launch hipTextureObject
// (dimension-agnostic). Upload via hipMemcpy3D from a linear host volume.
static int cajeta_hip_tex3d_supported(void) {
    return g_xpu_hip.hipMalloc3DArray && g_xpu_hip.hipMemcpy3D &&
           g_xpu_hip.hipCreateTextureObject && g_xpu_hip.hipDestroyTextureObject;
}

static int64_t cajeta_xpu_hip_tex3d_alloc(uint32_t w, uint32_t h, uint32_t d,
                                          int32_t format) {
    if (!cajeta_hip_tex3d_supported()) return 0;
    struct caj_hip_channel_format_desc cd = cajeta_hip_channel_desc(format);
    struct caj_hip_extent ext; ext.w = w; ext.h = h; ext.d = d;
    void* array = NULL;
    if (g_xpu_hip.hipMalloc3DArray(&array, &cd, ext, 0) != 0 || !array) return 0;
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) { if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(array); return 0; }
    t->array = array; t->mipmap = NULL; t->w = w; t->h = h; t->d = d;
    t->format = format; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

// Texture2DArray on AMD: a layered hipArray (hipMalloc3DArray + hipArrayLayered),
// whose extent.depth carries the LAYER count (not a true depth). The per-launch
// hipTextureObject is dimension-agnostic (RES_ARRAY), and the upload reuses the
// 3-D memcpy3D path with d = layers (a layered memcpy3D copies all layers).
static int64_t cajeta_xpu_hip_tex2darray_alloc(uint32_t w, uint32_t h,
                                               uint32_t layers, int32_t format) {
    if (!cajeta_hip_tex3d_supported()) return 0;
    struct caj_hip_channel_format_desc cd = cajeta_hip_channel_desc(format);
    struct caj_hip_extent ext; ext.w = w; ext.h = h; ext.d = layers;
    void* array = NULL;
    if (g_xpu_hip.hipMalloc3DArray(&array, &cd, ext, CAJ_HIP_ARRAY_LAYERED) != 0 ||
        !array)
        return 0;
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) { if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(array); return 0; }
    t->array = array; t->mipmap = NULL; t->w = w; t->h = h; t->d = layers;
    t->format = format; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

// TextureCube on AMD: a cubemap hipArray (hipMalloc3DArray + hipArrayCubemap),
// 6 faces in the extent's depth slot. Like the layered array, the texobj is
// dimension-agnostic (RES_ARRAY) and the upload reuses the 3-D memcpy3D path
// with d = 6 (a cubemap memcpy3D copies all 6 faces).
static int64_t cajeta_xpu_hip_texcube_alloc(uint32_t size, int32_t format) {
    if (!cajeta_hip_tex3d_supported()) return 0;
    struct caj_hip_channel_format_desc cd = cajeta_hip_channel_desc(format);
    struct caj_hip_extent ext; ext.w = size; ext.h = size; ext.d = 6;
    void* array = NULL;
    // NB: on gfx1151 / ROCm 7.2.2 this returns hipErrorInvalidValue — cubemap
    // arrays are unimplemented on this APU (as with mipmapped arrays), so cube
    // TextureCube on AMD degrades to "no device texture" (handle 0 → the kernel
    // doesn't launch). The path is correct for ROCm/hardware that supports cubemap
    // arrays.
    if (g_xpu_hip.hipMalloc3DArray(&array, &cd, ext, CAJ_HIP_ARRAY_CUBEMAP) != 0 ||
        !array)
        return 0;
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) { if (g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(array); return 0; }
    t->array = array; t->mipmap = NULL; t->w = size; t->h = size; t->d = 6;
    t->format = format; t->levels = 1;
    return (int64_t) (intptr_t) t;
}

static void cajeta_xpu_hip_tex3d_upload(int64_t handle, const float* src,
                                        uint32_t w, uint32_t h, uint32_t d,
                                        int32_t format) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) handle;
    if (!t || !t->array || w != t->w || h != t->h || d != t->d) return;
    size_t channels = (size_t) cajeta_texfmt_channels(format);
    size_t texelBytes = cajeta_texfmt_texel_bytes(format);
    size_t texels = (size_t) w * h * d * channels;
    // Encode into a packed temp for UNORM/half (1/2 bytes/channel); for f32 / raw
    // integer the float[] source bytes ARE the storage, so copy directly.
    void* hostBytes = (void*) src;
    void* tmp = NULL;
    if (cajeta_texfmt_is_unorm(format) || cajeta_texfmt_is_half(format)) {
        tmp = malloc(texels * cajeta_texfmt_channel_bytes(format));
        if (!tmp) return;
        cajeta_texfmt_encode(tmp, src, texels, format);
        hostBytes = tmp;
    }
    struct caj_hip_memcpy3d_parms p;
    memset(&p, 0, sizeof(p));
    p.srcPtr.ptr = hostBytes;
    p.srcPtr.pitch = (size_t) w * texelBytes;   // row pitch in bytes
    p.srcPtr.xsize = w;                          // logical width  (elements)
    p.srcPtr.ysize = h;                          // logical height (elements)
    p.dstArray = t->array;
    p.extent.w = w;                              // array-element extents
    p.extent.h = h;
    p.extent.d = d;
    p.kind = CAJ_HIP_MEMCPY_HTOD;
    g_xpu_hip.hipMemcpy3D(&p);
    if (tmp) free(tmp);
}

static void cajeta_xpu_hip_tex_upload(int64_t handle, const float* src,
                                      uint32_t w, uint32_t h, int32_t format) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) handle;
    if (!t || !t->array || w != t->w || h != t->h) return;
    size_t rowBytes = (size_t) w * cajeta_texfmt_texel_bytes(format);
    if (cajeta_texfmt_is_unorm(format) || cajeta_texfmt_is_half(format)) {
        // Storage differs from the float[] source (1 byte/sample UNORM, 2 bytes
        // half) — encode into a temp buffer, then copy the packed bytes.
        size_t texels = (size_t) w * h * cajeta_texfmt_channels(format);
        size_t bytes  = texels * cajeta_texfmt_channel_bytes(format);
        unsigned char* tmp = (unsigned char*) malloc(bytes);
        if (!tmp) return;
        cajeta_texfmt_encode(tmp, src, texels, format);
        g_xpu_hip.hipMemcpy2DToArray(t->array, 0, 0, tmp, rowBytes, rowBytes, h,
                                     CAJ_HIP_MEMCPY_HTOD);
        free(tmp);
    } else {
        g_xpu_hip.hipMemcpy2DToArray(t->array, 0, 0, src, rowBytes, rowBytes, h,
                                     CAJ_HIP_MEMCPY_HTOD);
    }
}

// Mipmapped Texture2D on AMD: a hipMipmappedArray (numLevels) whose per-level
// texels are staged by hipGetMipmappedArrayLevel → hipMemcpy2DToArray (the 2-D
// upload path per level); the per-launch hipTextureObject binds it via
// RES_MIPMAPPED_ARRAY (cajeta_xpu_hip_make_texobj).
static int cajeta_hip_tex_mip_supported(void) {
    return g_xpu_hip.hipMallocMipmappedArray &&
           g_xpu_hip.hipGetMipmappedArrayLevel && g_xpu_hip.hipMemcpy2DToArray &&
           g_xpu_hip.hipCreateTextureObject && g_xpu_hip.hipDestroyTextureObject;
}

static int64_t cajeta_xpu_hip_tex_alloc_mip(uint32_t w, uint32_t h, int32_t format,
                                            uint32_t levels) {
    if (!cajeta_hip_tex_mip_supported()) return 0;
    struct caj_hip_channel_format_desc cd = cajeta_hip_channel_desc(format);
    struct caj_hip_extent ext; ext.w = w; ext.h = h; ext.d = 0;  // 2-D mip array
    void* mipmap = NULL;
    // NB: on gfx1151 / ROCm 7.2 this returns hipErrorNotSupported (801) — AMD
    // mipmapped arrays are unimplemented on that APU, so mip Texture2D degrades to
    // "no device texture" (handle 0 → the kernel doesn't launch). The path is
    // correct for ROCm/hardware that does support mipmapped arrays.
    if (g_xpu_hip.hipMallocMipmappedArray(&mipmap, &cd, ext, levels, 0) != 0 ||
        !mipmap)
        return 0;
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) malloc(sizeof(*t));
    if (!t) {
        if (g_xpu_hip.hipFreeMipmappedArray) g_xpu_hip.hipFreeMipmappedArray(mipmap);
        return 0;
    }
    t->array = NULL; t->mipmap = mipmap; t->w = w; t->h = h; t->d = 1;
    t->format = format; t->levels = (int) levels;
    return (int64_t) (intptr_t) t;
}

static void cajeta_xpu_hip_tex_upload_level(int64_t handle, const float* src,
                                            uint32_t lw, uint32_t lh,
                                            uint32_t level, int32_t format) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) handle;
    if (!t || !t->mipmap || (int) level >= t->levels) return;
    void* levelArray = NULL;   // owned by the mipmapped array; not freed here
    if (g_xpu_hip.hipGetMipmappedArrayLevel(&levelArray, t->mipmap, level) != 0 ||
        !levelArray)
        return;
    size_t rowBytes = (size_t) lw * cajeta_texfmt_texel_bytes(format);
    if (cajeta_texfmt_is_unorm(format) || cajeta_texfmt_is_half(format)) {
        size_t texels = (size_t) lw * lh * cajeta_texfmt_channels(format);
        unsigned char* tmp =
            (unsigned char*) malloc(texels * cajeta_texfmt_channel_bytes(format));
        if (!tmp) return;
        cajeta_texfmt_encode(tmp, src, texels, format);
        g_xpu_hip.hipMemcpy2DToArray(levelArray, 0, 0, tmp, rowBytes, rowBytes, lh,
                                     CAJ_HIP_MEMCPY_HTOD);
        free(tmp);
    } else {
        g_xpu_hip.hipMemcpy2DToArray(levelArray, 0, 0, src, rowBytes, rowBytes, lh,
                                     CAJ_HIP_MEMCPY_HTOD);
    }
}

static void cajeta_xpu_hip_tex_free(int64_t handle) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) handle;
    if (!t) return;
    if (t->array && g_xpu_hip.hipFreeArray) g_xpu_hip.hipFreeArray(t->array);
    if (t->mipmap && g_xpu_hip.hipFreeMipmappedArray)
        g_xpu_hip.hipFreeMipmappedArray(t->mipmap);
    free(t);
}

// Build a hipTextureObject from a texture record's array + a cajeta Sampler's
// modes (filterMode 0=nearest/1=linear, addressMode 0=clamp/1=wrap), normalized
// coords, element-type read. Returns the object pointer (as int64) or 0.
static int64_t cajeta_xpu_hip_make_texobj(int64_t texHandle, int32_t filterMode,
                                          int32_t addressMode) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) texHandle;
    if (!t || (!t->array && !t->mipmap) || !cajeta_hip_tex_supported()) return 0;
    struct caj_hip_resource_desc rd;
    memset(&rd, 0, sizeof(rd));
    if (t->mipmap) {   // mip Texture2D — bind the whole mipmapped array
        rd.resType = CAJ_HIP_RES_MIPMAPPED_ARRAY;
        rd.res.mipmap.mipmap = t->mipmap;
    } else {
        rd.resType = CAJ_HIP_RES_ARRAY;
        rd.res.array.array = t->array;
    }
    struct caj_hip_texture_desc td;
    memset(&td, 0, sizeof(td));
    int hipAddr = addressMode == 1 ? CAJ_HIP_ADDR_WRAP : CAJ_HIP_ADDR_CLAMP;
    td.addressMode[0] = hipAddr; td.addressMode[1] = hipAddr;
    td.addressMode[2] = hipAddr;
    int hipFilter = filterMode == 1 ? CAJ_HIP_FILTER_LINEAR : CAJ_HIP_FILTER_POINT;
    td.filterMode = hipFilter;
    // UNORM arrays read back as normalized float [0,1]; float arrays read raw.
    td.readMode = cajeta_texfmt_is_unorm(t->format) ? CAJ_HIP_READ_NORMALIZED_FLOAT
                                                    : CAJ_HIP_READ_ELEMENT;
    td.normalizedCoords = 1;
    // Mip clamp: maxMipmapLevelClamp must admit the highest level an explicit-LOD
    // sample can request — 0 would clamp every __ockl_image_sample_lod_2D to level
    // 0 (the AMD analog of the Vulkan sampler maxLod=0 bug). Inter-level filter
    // mirrors the magnify filter; harmless for non-mip (levels=1 → clamp 0).
    td.mipmapFilterMode = hipFilter;
    td.minMipmapLevelClamp = 0.0f;
    td.maxMipmapLevelClamp = t->levels > 1 ? (float) (t->levels - 1) : 0.0f;
    void* texObj = NULL;
    if (g_xpu_hip.hipCreateTextureObject(&texObj, &rd, &td, NULL) != 0)
        return 0;
    return (int64_t) (intptr_t) texObj;
}

// Build a SURFACE object for an Image2D storage image (the writable twin of
// make_texobj). Just the ARRAY resource desc — no sampler, no read mode (a
// surface read/write is raw). The kernel consumes it via __ockl_image_store_2D /
// __ockl_image_load_2D. Returns 0 if surfaces are unsupported or creation fails.
static int64_t cajeta_xpu_hip_make_surfobj(int64_t imgHandle) {
    struct cajeta_hip_tex* t = (struct cajeta_hip_tex*) (intptr_t) imgHandle;
    if (!t || !t->array || !cajeta_hip_surf_supported()) return 0;
    struct caj_hip_resource_desc rd;
    memset(&rd, 0, sizeof(rd));
    rd.resType = CAJ_HIP_RES_ARRAY;
    rd.res.array.array = t->array;
    void* surfObj = NULL;
    if (g_xpu_hip.hipCreateSurfaceObject(&surfObj, &rd) != 0) return 0;
    return (int64_t) (intptr_t) surfObj;
}

// --- Texture2D + Sampler (Item 8) -------------------------------------------
// A Texture2D is a small host-side handle (deviceHandle + width/height) over a
// device image; on the CPU backend the device image IS a host allocation. The
// int64 deviceHandle is a pointer to this texobj — a row-major float32 image
// with its dimensions — exactly as a Buffer's handle is its host block. The
// kernel receives that pointer (marshalled like a buffer) and reads it through
// __cajeta_xpu_cpu_tex_sample, which does the addressing + filtering the GPU
// texture unit would. On Vulkan/AMD the handle is a device image / hipArray
// record and the kernel samples it through the native image path.
#define CAJ_MAX_MIP 16
struct cajeta_cpu_texobj {
    float*   data;     // row-major DECODED float texels (owned). Level 0 starts at
                       // offset 0 (mipoff[0]=0), so non-mip code reads t->data
                       // directly; mip levels follow at mipoff[l].
    uint32_t w;        // level-0 width
    uint32_t h;        // level-0 height
    uint32_t d;        // depth: 1 for a 2-D texture, >=1 for a 3-D volume
    int32_t  format;   // TextureFormat ordinal
    int      channels; // 1 (R) or 4 (RGBA)
    int      levels;   // mip level count (1 = no mipmaps)
    size_t   mipoff[CAJ_MAX_MIP];  // element offset (in floats) of each mip level
    uint32_t mipw[CAJ_MAX_MIP], miph[CAJ_MAX_MIP];  // per-level dims
};

// __cajeta_xpu_texture_alloc(this, width, height) -> int64 handle.
// Instance @Native (the Buffer convention): the leading `self` is the cajeta
// `this`, ignored — the device side is keyed on the returned handle.
int64_t __cajeta_xpu_texture_alloc(void* self, uint32_t width, uint32_t height,
                                   int32_t format) {
    (void) self;
    if (width == 0 || height == 0) return 0;
    int channels = cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) malloc(sizeof(*t));
            if (!t) return 0;
            t->w = width;
            t->h = height;
            t->d = 1;            // 2-D texture: single depth slice
            t->format = format;
            t->channels = channels;
            t->levels = 1;       // no mipmaps; level 0 at offset 0
            t->mipoff[0] = 0; t->mipw[0] = width; t->miph[0] = height;
            // CPU stores DECODED channel-interleaved floats (channels per texel);
            // UNORM precision is emulated at upload, so the sampler is float-only.
            t->data = (float*) calloc((size_t) width * height * channels,
                                      sizeof(float));
            if (!t->data) { free(t); return 0; }
            return (int64_t) (intptr_t) t;
        }
        case CAJ_XPU_VULKAN:
            return cajeta_xpu_vk_tex_alloc(width, height, 0, format, 1, 2, 1, 1); // sampled 2-D
        case CAJ_XPU_HIP:
            return cajeta_xpu_hip_tex_alloc(width, height, format);   // hipArray
        // CUDA texture objects land in Stage D (emit-only).
        default: return 0;
    }
}

// __cajeta_xpu_texture_upload(this, handle, host, width, height).
// `host` is a Cajeta float32[] header — { i64 count, [count x f32] data } — so
// the texels start at offset 8 (matches __cajeta_xpu_buffer_upload).
void __cajeta_xpu_texture_upload(void* self, int64_t handle, void* host,
                                 uint32_t width, uint32_t height, int32_t format) {
    (void) self;
    if (!handle || !host || width == 0 || height == 0) return;
    // `host` is a cajeta float32[] header { i64 count, [count x f32] } — the
    // channel-interleaved float texels (R, or R,G,B,A per texel) start at offset 8.
    const float* src = (const float*) ((const char*) host + 8);
    size_t texels = (size_t) width * height * cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            if (!t->data) return;
            if (cajeta_texfmt_is_unorm(format)) {
                // Emulate the device's 256-level UNORM quantization on the CPU so
                // both paths agree bit-for-bit on exactly-representable values.
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = (float) cajeta_texfmt_unorm8(src[i]) / 255.0f;
            } else if (cajeta_texfmt_is_half(format)) {
                // Emulate binary16 storage precision (round-trip through f16) so
                // the CPU path matches the device's half rounding.
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = cajeta_f16_to_f32(cajeta_f32_to_f16(src[i]));
            } else {
                memcpy(t->data, src, texels * sizeof(float));
            }
            return;
        }
        case CAJ_XPU_VULKAN:
            cajeta_xpu_vk_tex_upload(handle, src, width, height, format);
            return;
        case CAJ_XPU_HIP:
            cajeta_xpu_hip_tex_upload(handle, src, width, height, format);
            return;
        default: return;
    }
}

void __cajeta_xpu_texture_free(void* self, int64_t handle) {
    (void) self;
    if (!handle) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            free(t->data);
            free(t);
            return;
        }
        case CAJ_XPU_VULKAN: cajeta_xpu_vk_tex_free(handle); return;
        case CAJ_XPU_HIP:    cajeta_xpu_hip_tex_free(handle); return;
        default: return;
    }
}

// --- Mipmapped Texture2D ----------------------------------------------------
// A mip chain: level 0 = w x h, level L = max(1, w>>L) x max(1, h>>L). The CPU
// stores all levels in one buffer with per-level offsets (level 0 at offset 0, so
// the non-mip read path is unchanged). Vulkan/HIP mip paths land in later
// increments (default = 1-level fallback so the drop chain still works).

// __cajeta_xpu_texture_alloc_mip(this, w, h, format, levels) -> handle.
int64_t __cajeta_xpu_texture_alloc_mip(void* self, uint32_t width, uint32_t height,
                                       int32_t format, uint32_t levels) {
    (void) self;
    if (width == 0 || height == 0 || levels == 0) return 0;
    if (levels > CAJ_MAX_MIP) levels = CAJ_MAX_MIP;
    int channels = cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) malloc(sizeof(*t));
            if (!t) return 0;
            t->w = width; t->h = height; t->d = 1;
            t->format = format; t->channels = channels;
            t->levels = (int) levels;
            // Lay the mip levels out back-to-back; record each offset + dims.
            size_t off = 0;
            for (uint32_t l = 0; l < levels; ++l) {
                uint32_t lw = width >> l;  if (lw == 0) lw = 1;
                uint32_t lh = height >> l; if (lh == 0) lh = 1;
                t->mipoff[l] = off;
                t->mipw[l] = lw; t->miph[l] = lh;
                off += (size_t) lw * lh * channels;
            }
            t->data = (float*) calloc(off, sizeof(float));
            if (!t->data) { free(t); return 0; }
            return (int64_t) (intptr_t) t;
        }
        case CAJ_XPU_VULKAN:
            // A sampled 2-D image with `levels` mip levels; per-level texels are
            // staged by __cajeta_xpu_texture_upload_level.
            return cajeta_xpu_vk_tex_alloc(width, height, 0, format, 1, 2, 1, levels);
        case CAJ_XPU_HIP:
            return cajeta_xpu_hip_tex_alloc_mip(width, height, format, levels);
        default: return 0;
    }
}

// __cajeta_xpu_texture_upload_level(this, handle, host, lw, lh, level, format).
void __cajeta_xpu_texture_upload_level(void* self, int64_t handle, void* host,
                                       uint32_t lw, uint32_t lh, uint32_t level,
                                       int32_t format) {
    (void) self;
    if (!handle || !host || lw == 0 || lh == 0) return;
    const float* src = (const float*) ((const char*) host + 8);
    size_t texels = (size_t) lw * lh * cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            if (!t->data || (int) level >= t->levels) return;
            float* dst = t->data + t->mipoff[level];
            if (cajeta_texfmt_is_unorm(format)) {
                for (size_t i = 0; i < texels; ++i)
                    dst[i] = (float) cajeta_texfmt_unorm8(src[i]) / 255.0f;
            } else if (cajeta_texfmt_is_half(format)) {
                for (size_t i = 0; i < texels; ++i)
                    dst[i] = cajeta_f16_to_f32(cajeta_f32_to_f16(src[i]));
            } else {
                memcpy(dst, src, texels * sizeof(float));
            }
            return;
        }
        case CAJ_XPU_VULKAN:
            cajeta_xpu_vk_tex_upload_level(handle, src, lw, lh, level, format);
            return;
        case CAJ_XPU_HIP:
            cajeta_xpu_hip_tex_upload_level(handle, src, lw, lh, level, format);
            return;
        default: return;
    }
}

// --- Texture3D (3-D / volumetric textures) ----------------------------------
// The volumetric sibling of Texture2D. Distinct __cajeta_xpu_texture3d_* symbols
// because the 2-D vs 3-D image type (VK_IMAGE_TYPE_3D, hipMalloc3DArray) is fixed
// at allocation. CPU stores a w*h*d*channels DECODED-float volume (row-major: x
// fastest, then y, then z). Vulkan/HIP 3-D image paths land in later increments
// (default = no-op / 0 until then, so the cajeta drop chain still works).

// __cajeta_xpu_texture3d_alloc(this, width, height, depth, format) -> handle.
int64_t __cajeta_xpu_texture3d_alloc(void* self, uint32_t width, uint32_t height,
                                     uint32_t depth, int32_t format) {
    (void) self;
    if (width == 0 || height == 0 || depth == 0) return 0;
    int channels = cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) malloc(sizeof(*t));
            if (!t) return 0;
            t->w = width;
            t->h = height;
            t->d = depth;
            t->format = format;
            t->channels = channels;
            t->levels = 1;
            t->mipoff[0] = 0; t->mipw[0] = width; t->miph[0] = height;
            t->data = (float*) calloc(
                (size_t) width * height * depth * channels, sizeof(float));
            if (!t->data) { free(t); return 0; }
            return (int64_t) (intptr_t) t;
        }
        case CAJ_XPU_VULKAN:
            return cajeta_xpu_vk_tex_alloc(width, height, 0, format, depth, 3, 1, 1);
        case CAJ_XPU_HIP:
            return cajeta_xpu_hip_tex3d_alloc(width, height, depth, format);
        default: return 0;
    }
}

// __cajeta_xpu_texture3d_upload(this, handle, host, width, height, depth, format).
void __cajeta_xpu_texture3d_upload(void* self, int64_t handle, void* host,
                                   uint32_t width, uint32_t height, uint32_t depth,
                                   int32_t format) {
    (void) self;
    if (!handle || !host || width == 0 || height == 0 || depth == 0) return;
    const float* src = (const float*) ((const char*) host + 8);
    size_t texels = (size_t) width * height * depth * cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            if (!t->data) return;
            if (cajeta_texfmt_is_unorm(format)) {
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = (float) cajeta_texfmt_unorm8(src[i]) / 255.0f;
            } else if (cajeta_texfmt_is_half(format)) {
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = cajeta_f16_to_f32(cajeta_f32_to_f16(src[i]));
            } else {
                memcpy(t->data, src, texels * sizeof(float));
            }
            return;
        }
        case CAJ_XPU_VULKAN:
            // cajeta_xpu_vk_tex_upload reads the depth from the texture record,
            // so the 2-D upload path covers 3-D images unchanged.
            cajeta_xpu_vk_tex_upload(handle, src, width, height, format);
            return;
        case CAJ_XPU_HIP:
            cajeta_xpu_hip_tex3d_upload(handle, src, width, height, depth, format);
            return;
        default: return;
    }
}

void __cajeta_xpu_texture3d_free(void* self, int64_t handle) {
    (void) self;
    if (!handle) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            free(t->data);
            free(t);
            return;
        }
        case CAJ_XPU_VULKAN: cajeta_xpu_vk_tex_free(handle); return;
        case CAJ_XPU_HIP:    cajeta_xpu_hip_tex_free(handle); return;
        default: return;
    }
}

// --- Texture1D (read-only 1-D images) ---------------------------------------
// Texture1D is the linear sibling of Texture2D/Texture3D: a single (width) row.
// On the CPU it is exactly a 2-D texobj with height = 1, so the alloc/upload
// below build that shape and every CPU read reuses the 2-D sample/fetch path
// (the 2-D bilinear collapses to a 1-D lerp when there is one row). Vulkan/HIP
// are stubbed for 3a and wired in 3b/3c.

// __cajeta_xpu_texture1d_alloc(this, width, format) -> handle.
int64_t __cajeta_xpu_texture1d_alloc(void* self, uint32_t width, int32_t format) {
    (void) self;
    if (width == 0) return 0;
    int channels = cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) malloc(sizeof(*t));
            if (!t) return 0;
            t->w = width;
            t->h = 1;
            t->d = 1;
            t->format = format;
            t->channels = channels;
            t->levels = 1;
            t->mipoff[0] = 0; t->mipw[0] = width; t->miph[0] = 1;
            t->data = (float*) calloc((size_t) width * channels, sizeof(float));
            if (!t->data) { free(t); return 0; }
            return (int64_t) (intptr_t) t;
        }
        case CAJ_XPU_VULKAN:
            // A 1-D sampled image (height = depth = 1, no mips).
            return cajeta_xpu_vk_tex_alloc(width, 1, 0, format, 1, 1, 1, 1);
        case CAJ_XPU_HIP:    return cajeta_xpu_hip_tex1d_alloc(width, format);
        default: return 0;
    }
}

// __cajeta_xpu_texture1d_upload(this, handle, host, width, format).
void __cajeta_xpu_texture1d_upload(void* self, int64_t handle, void* host,
                                   uint32_t width, int32_t format) {
    (void) self;
    if (!handle || !host || width == 0) return;
    const float* src = (const float*) ((const char*) host + 8);
    size_t texels = (size_t) width * cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            if (!t->data) return;
            if (cajeta_texfmt_is_unorm(format)) {
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = (float) cajeta_texfmt_unorm8(src[i]) / 255.0f;
            } else if (cajeta_texfmt_is_half(format)) {
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = cajeta_f16_to_f32(cajeta_f32_to_f16(src[i]));
            } else {
                memcpy(t->data, src, texels * sizeof(float));
            }
            return;
        }
        case CAJ_XPU_VULKAN:
            // A 1-D image is height 1; the upload reads depth (= 1) from the record.
            cajeta_xpu_vk_tex_upload(handle, src, width, 1, format);
            return;
        case CAJ_XPU_HIP:
            // A 1-D hipArray is a height-1 2-D copy — reuse the 2-D upload.
            cajeta_xpu_hip_tex_upload(handle, src, width, 1, format);
            return;
        default: return;
    }
}

void __cajeta_xpu_texture1d_free(void* self, int64_t handle) {
    (void) self;
    if (!handle) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            free(t->data);
            free(t);
            return;
        }
        case CAJ_XPU_VULKAN: cajeta_xpu_vk_tex_free(handle); return;
        case CAJ_XPU_HIP:    cajeta_xpu_hip_tex_free(handle); return;
        default: return;
    }
}

// --- Texture2DArray (read-only layered 2-D images) --------------------------
// A 2-D array is N (width, height) planes. On the CPU it is a cajeta_cpu_texobj
// whose `d` field is the layer count and whose storage is laid out exactly like
// a 3-D volume's z slices (so the CPU fetch reuses the 3-D path with z = layer,
// and sample bilinearly filters within one layer). Vulkan/HIP layered images are
// wired in A2/A3.

// __cajeta_xpu_texture2darray_alloc(this, width, height, layers, format) -> handle.
int64_t __cajeta_xpu_texture2darray_alloc(void* self, uint32_t width,
                                          uint32_t height, uint32_t layers,
                                          int32_t format) {
    (void) self;
    if (width == 0 || height == 0 || layers == 0) return 0;
    int channels = cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) malloc(sizeof(*t));
            if (!t) return 0;
            t->w = width;
            t->h = height;
            t->d = layers;       // layer count stored in the volume's depth slot
            t->format = format;
            t->channels = channels;
            t->levels = 1;
            t->mipoff[0] = 0; t->mipw[0] = width; t->miph[0] = height;
            t->data = (float*) calloc(
                (size_t) width * height * layers * channels, sizeof(float));
            if (!t->data) { free(t); return 0; }
            return (int64_t) (intptr_t) t;
        }
        case CAJ_XPU_VULKAN:
            // A layered 2-D sampled image: imageKind 4, arrayLayers = layers.
            return cajeta_xpu_vk_tex_alloc(width, height, 0, format, 1, 4, layers, 1);
        case CAJ_XPU_HIP:
            return cajeta_xpu_hip_tex2darray_alloc(width, height, layers, format);
        default: return 0;
    }
}

// __cajeta_xpu_texture2darray_upload(this, handle, host, width, height, layers, format).
void __cajeta_xpu_texture2darray_upload(void* self, int64_t handle, void* host,
                                        uint32_t width, uint32_t height,
                                        uint32_t layers, int32_t format) {
    (void) self;
    if (!handle || !host || width == 0 || height == 0 || layers == 0) return;
    const float* src = (const float*) ((const char*) host + 8);
    size_t texels =
        (size_t) width * height * layers * cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            if (!t->data) return;
            if (cajeta_texfmt_is_unorm(format)) {
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = (float) cajeta_texfmt_unorm8(src[i]) / 255.0f;
            } else if (cajeta_texfmt_is_half(format)) {
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = cajeta_f16_to_f32(cajeta_f32_to_f16(src[i]));
            } else {
                memcpy(t->data, src, texels * sizeof(float));
            }
            return;
        }
        case CAJ_XPU_VULKAN:
            // The layered image carries its planes in array layers; the upload
            // reads layer count + layered flag from the texture record.
            cajeta_xpu_vk_tex_upload(handle, src, width, height, format);
            return;
        case CAJ_XPU_HIP:
            // A layered hipArray's memcpy3D copies all layers with d = layers.
            cajeta_xpu_hip_tex3d_upload(handle, src, width, height, layers, format);
            return;
        default: return;
    }
}

void __cajeta_xpu_texture2darray_free(void* self, int64_t handle) {
    (void) self;
    if (!handle) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            free(t->data);
            free(t);
            return;
        }
        case CAJ_XPU_VULKAN: cajeta_xpu_vk_tex_free(handle); return;
        case CAJ_XPU_HIP:    cajeta_xpu_hip_tex_free(handle); return;
        default: return;
    }
}

// --- TextureCube (read-only cube maps, 6 faces) -----------------------------
// A cube is 6 square faces in +X,-X,+Y,-Y,+Z,-Z order. On the CPU it is a
// cajeta_cpu_texobj whose `d` is 6 (the faces are the z slices), so the CPU
// sampler reuses the volume storage + does the direction→face projection.
// Vulkan/HIP cube images are wired in B2/B3.

// __cajeta_xpu_texturecube_alloc(this, size, format) -> handle.
int64_t __cajeta_xpu_texturecube_alloc(void* self, uint32_t size, int32_t format) {
    (void) self;
    if (size == 0) return 0;
    int channels = cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) malloc(sizeof(*t));
            if (!t) return 0;
            t->w = size;
            t->h = size;
            t->d = 6;            // the 6 faces are the z slices
            t->format = format;
            t->channels = channels;
            t->levels = 1;
            t->mipoff[0] = 0; t->mipw[0] = size; t->miph[0] = size;
            t->data = (float*) calloc(
                (size_t) size * size * 6 * channels, sizeof(float));
            if (!t->data) { free(t); return 0; }
            return (int64_t) (intptr_t) t;
        }
        case CAJ_XPU_VULKAN:
            // A CUBE_COMPATIBLE 2-D image with 6 array layers (the faces).
            return cajeta_xpu_vk_tex_alloc(size, size, 0, format, 1, 5, 6, 1);
        case CAJ_XPU_HIP:    return cajeta_xpu_hip_texcube_alloc(size, format);
        default: return 0;
    }
}

// __cajeta_xpu_texturecube_upload(this, handle, host, size, format).
void __cajeta_xpu_texturecube_upload(void* self, int64_t handle, void* host,
                                     uint32_t size, int32_t format) {
    (void) self;
    if (!handle || !host || size == 0) return;
    const float* src = (const float*) ((const char*) host + 8);
    size_t texels = (size_t) size * size * 6 * cajeta_texfmt_channels(format);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            if (!t->data) return;
            if (cajeta_texfmt_is_unorm(format)) {
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = (float) cajeta_texfmt_unorm8(src[i]) / 255.0f;
            } else if (cajeta_texfmt_is_half(format)) {
                for (size_t i = 0; i < texels; ++i)
                    t->data[i] = cajeta_f16_to_f32(cajeta_f32_to_f16(src[i]));
            } else {
                memcpy(t->data, src, texels * sizeof(float));
            }
            return;
        }
        case CAJ_XPU_VULKAN:
            // The 6 faces are the image's 6 array layers; the upload reads the
            // layer count + layered flag from the texture record (layers = 6).
            cajeta_xpu_vk_tex_upload(handle, src, size, size, format);
            return;
        case CAJ_XPU_HIP:
            // A cubemap hipArray's memcpy3D copies all 6 faces with d = 6.
            cajeta_xpu_hip_tex3d_upload(handle, src, size, size, 6, format);
            return;
        default: return;
    }
}

void __cajeta_xpu_texturecube_free(void* self, int64_t handle) {
    (void) self;
    if (!handle) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: {
            struct cajeta_cpu_texobj* t =
                (struct cajeta_cpu_texobj*) (intptr_t) handle;
            free(t->data);
            free(t);
            return;
        }
        case CAJ_XPU_VULKAN: cajeta_xpu_vk_tex_free(handle); return;
        case CAJ_XPU_HIP:    cajeta_xpu_hip_tex_free(handle); return;
        default: return;
    }
}

// --- Image2D (writable storage images) --------------------------------------
// Image2D is the writable twin of Texture2D: a 2-D R32_SFLOAT storage image a
// kernel writes via `img.store(x, y, value)` (OpImageWrite), and the host reads
// back with `img.download(out)`. Vulkan (storage image), AMD (surface object),
// and CPU (the reference host float store); NV returns 0 / no-op so the cajeta
// drop chain still works. The handle is a backend-specific record/index.

// CPU Image2D: the in-process reference store — a flat R32f host float array in a
// cajeta_cpu_texobj (channels=1), the writable twin of the CPU texture path. The
// device kernel writes/reads it via __cajeta_xpu_cpu_image_store/_load below.
static int64_t cajeta_xpu_cpu_image_alloc(uint32_t w, uint32_t h) {
    struct cajeta_cpu_texobj* t =
        (struct cajeta_cpu_texobj*) malloc(sizeof(*t));
    if (!t) return 0;
    t->w = w; t->h = h; t->d = 1;
    t->format = CAJ_TEXFMT_R32F; t->channels = 1; t->levels = 1;
    t->mipoff[0] = 0; t->mipw[0] = w; t->miph[0] = h;
    t->data = (float*) calloc((size_t) w * h, sizeof(float));
    if (!t->data) { free(t); return 0; }
    return (int64_t) (intptr_t) t;
}

static void cajeta_xpu_cpu_image_download(int64_t handle, void* host,
                                          uint32_t w, uint32_t h) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) (intptr_t) handle;
    if (!t || !t->data || !host) return;
    memcpy(host, t->data, (size_t) w * h * sizeof(float));
}

static void cajeta_xpu_cpu_image_free(int64_t handle) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) (intptr_t) handle;
    if (!t) return;
    free(t->data);
    free(t);
}

// __cajeta_xpu_image_alloc(this, width, height) -> int64 handle.
int64_t __cajeta_xpu_image_alloc(void* self, uint32_t width, uint32_t height) {
    (void) self;
    if (width == 0 || height == 0) return 0;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU:
            return cajeta_xpu_cpu_image_alloc(width, height);  // host float store (R32F)
        case CAJ_XPU_VULKAN:
            return cajeta_xpu_vk_tex_alloc(width, height, 1, CAJ_TEXFMT_R32F, 1, 2, 1, 1);  // storage 2-D (R32F)
        case CAJ_XPU_HIP:
            return cajeta_xpu_hip_image_alloc(width, height);  // surface hipArray (R32F)
        default: return 0;
    }
}

// __cajeta_xpu_image_download(this, handle, host, width, height).
// `host` is a Cajeta float32[] header — { i64 count, [count x f32] data } — so
// the texels land at offset 8 (matches __cajeta_xpu_texture_upload in reverse).
void __cajeta_xpu_image_download(void* self, int64_t handle, void* host,
                                 uint32_t width, uint32_t height) {
    (void) self;
    if (!handle || !host || width == 0 || height == 0) return;
    void* data = (void*) ((char*) host + 8);
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU:
            cajeta_xpu_cpu_image_download(handle, data, width, height);
            return;
        case CAJ_XPU_VULKAN:
            cajeta_xpu_vk_tex_download(handle, data, width, height);
            return;
        case CAJ_XPU_HIP:
            cajeta_xpu_hip_image_download(handle, data, width, height);
            return;
        default: return;
    }
}

void __cajeta_xpu_image_free(void* self, int64_t handle) {
    (void) self;
    if (!handle) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CPU: cajeta_xpu_cpu_image_free(handle); return;
        case CAJ_XPU_VULKAN: cajeta_xpu_vk_tex_free(handle); return;
        case CAJ_XPU_HIP: cajeta_xpu_hip_image_free(handle); return;
        default: return;
    }
}

// Portable software BVH builder + layout (the software AccelerationStructure
// noun). Self-contained pure C, also compiled directly by the builder unit test.
#include "cajeta_bvh.c"

// --- Noun seam: the resource-provider SPI (cajeta-gpu inc-4 brick #2) --------
//
// The first-class mirror of the verb seam (LoweringTarget): a struct of build/
// free hooks, one instance per backend, through which a core noun is built from
// its description. This is the machinery a vendor extension implements for a
// noun (VendorExtensionSDK.md §2) — build-from-description, not convert-between-
// builts. Dogfooded on AccelerationStructure: the seam-defining noun
// (CajetaGPU.md §4) and the only noun with impl divergence (software BVH vs
// native BLAS). Buffer/Texture/Image have one impl per backend, so their slots
// are reserved here but stay on their existing switch dispatch (no tag would be
// meaningful). Each build reports the CajetaAsImpl it used; the noun records it;
// free follows the RECORDED impl, not the active backend.
#include "cajeta_noun_impl.h"

// Native inline ray query available on the active device? (Same condition as
// __cajeta_xpu_device_supports(RayQueryNative).) The native-vs-software input.
static int caj_native_rayquery_available(void) {
#if defined(CAJETA_RT_HAS_VULKAN) && !defined(_WIN32)
    return (cajeta_xpu_active_backend() == CAJ_XPU_VULKAN && g_xpu_vk.rayQuery) ? 1 : 0;
#else
    return 0;
#endif
}

// Resolve an AS impl preference (CajetaAsPref) to a concrete impl (inc-4 brick #3).
// Precedence: the CAJETA_GPU_AS_IMPL env override wins, then the explicit
// preference, then the AUTO default policy. Read once per call (constant within a
// run), so the build's choice and the recorded impl always agree. A NATIVE request
// with no native support falls back to the software floor (core always runs).
// This is the RUNTIME-NOUN instance of the CAJETA_GPU_<FEATURE>_IMPL degrade-
// override convention (inc-4 brick #4); the compile-time-feature instance is
// resolveImplTier() in src/cajeta/xpu/lowering/KernelLowering.cpp (e.g.
// CAJETA_GPU_COOPMATRIX_IMPL). Same precedence + case-sensitive string match.
static CajetaAsImpl caj_resolve_as_impl(int pref) {
    int native = caj_native_rayquery_available();
    const char* env = getenv("CAJETA_GPU_AS_IMPL");
    if (env && *env) {
        if (strcmp(env, "software") == 0) return CAJ_AS_IMPL_SOFTWARE_BVH;
        if (strcmp(env, "native") == 0)
            return native ? CAJ_AS_IMPL_VULKAN_NATIVE : CAJ_AS_IMPL_SOFTWARE_BVH;
        // unknown value: ignore; fall through to the explicit preference.
    }
    if (pref == CAJ_AS_PREF_SOFTWARE) return CAJ_AS_IMPL_SOFTWARE_BVH;
    if (pref == CAJ_AS_PREF_NATIVE)
        return native ? CAJ_AS_IMPL_VULKAN_NATIVE : CAJ_AS_IMPL_SOFTWARE_BVH;
    return caj_default_as_impl(native);   // AUTO
}

typedef struct CajetaNounProvider {
    const char*  name;
    int          backend_id;
    // AccelerationStructure noun (wired). `pref` is the CajetaAsPref override;
    // out_impl reports the impl the build actually chose (after resolution).
    int64_t      (*accel_build_aabbs)(const float* boxes, uint32_t count,
                                      int32_t pref, CajetaAsImpl* out_impl);
    int64_t      (*accel_build_triangles)(const float* verts, uint32_t triCount,
                                          uint32_t stride, CajetaAsImpl* out_impl);
    void         (*accel_free)(int64_t handle, CajetaAsImpl impl);
    // Buffer / Texture / Image noun slots: reserved (the unified contract);
    // routed by their existing dispatchers until they gain impl divergence.
} CajetaNounProvider;

// CPU provider — the portable software BVH (the floor; handle == host blob ptr).
// The CPU backend has only the software impl, so `pref` is moot here.
static int64_t caj_cpu_accel_build_aabbs(const float* boxes, uint32_t count,
                                         int32_t pref, CajetaAsImpl* out_impl) {
    (void) pref;
    if (out_impl) *out_impl = CAJ_AS_IMPL_SOFTWARE_BVH;
    return cajeta_xpu_cpu_accel_build_aabbs(boxes, count);
}
static int64_t caj_cpu_accel_build_triangles(const float* verts, uint32_t triCount,
                                             uint32_t stride, CajetaAsImpl* out_impl) {
    if (out_impl) *out_impl = CAJ_AS_IMPL_SOFTWARE_BVH;
    return cajeta_xpu_cpu_accel_build_triangles(verts, triCount, stride);
}
static void caj_cpu_accel_free(int64_t handle, CajetaAsImpl impl) {
    (void) impl;
    free((void*) (intptr_t) handle);
}

static const CajetaNounProvider caj_cpu_noun_provider = {
    "cpu", CAJ_XPU_CPU,
    caj_cpu_accel_build_aabbs, caj_cpu_accel_build_triangles,
    caj_cpu_accel_free,
};

// Vulkan provider — native VK_KHR_acceleration_structure BLAS, or (forced/auto-
// software) the portable software BVH uploaded into a storage buffer the "<name>$sw"
// kernel variant reads as bvh[i]. The resolved impl drives both.
static int64_t caj_vk_accel_build_aabbs(const float* boxes, uint32_t count,
                                        int32_t pref, CajetaAsImpl* out_impl) {
    CajetaAsImpl impl = caj_resolve_as_impl(pref);
    if (out_impl) *out_impl = impl;
    if (impl == CAJ_AS_IMPL_SOFTWARE_BVH) {
        int64_t blob = cajeta_xpu_cpu_accel_build_aabbs(boxes, count);  // host blob
        if (!blob) return 0;
        const float* hdr = (const float*) (intptr_t) blob;
        uint64_t bytes = (uint64_t) caj_bvh_block_words(hdr) * 4u;
        int64_t buf = cajeta_xpu_vk_alloc(bytes);
        if (buf) {
            void* m = cajeta_xpu_vk_mapped(buf);
            if (m) memcpy(m, hdr, (size_t) bytes);
            else { cajeta_xpu_vk_free(buf); buf = 0; }
        }
        free((void*) (intptr_t) blob);
        return buf;
    }
    return cajeta_xpu_vk_accel_build_aabbs(boxes, count);  // native BLAS
}
static int64_t caj_vk_accel_build_triangles(const float* verts, uint32_t triCount,
                                            uint32_t stride, CajetaAsImpl* out_impl) {
    // Triangle forced-software-on-Vulkan is deferred (AABB-only this brick); the
    // Vulkan triangle path is the native BLAS.
    if (out_impl) *out_impl = CAJ_AS_IMPL_VULKAN_NATIVE;
    return cajeta_xpu_vk_accel_build_triangles(verts, triCount, stride);
}
static void caj_vk_accel_free(int64_t handle, CajetaAsImpl impl) {
    // Free follows the recorded impl: a software BVH is a storage buffer; a native
    // BLAS is an accel-table entry.
    if (impl == CAJ_AS_IMPL_SOFTWARE_BVH) cajeta_xpu_vk_free(handle);
    else cajeta_xpu_vk_accel_free(handle);
}

static const CajetaNounProvider caj_vk_noun_provider = {
    "vulkan", CAJ_XPU_VULKAN,
    caj_vk_accel_build_aabbs, caj_vk_accel_build_triangles,
    caj_vk_accel_free,
};

// Registry indexed by backend id. CUDA/HIP have no device AS yet (software-BVH-
// on-device is a follow-up): NULL there.
static const CajetaNounProvider* const g_xpu_noun_providers[CAJ_XPU_COUNT] = {
    [CAJ_XPU_VULKAN] = &caj_vk_noun_provider,
    [CAJ_XPU_CPU]    = &caj_cpu_noun_provider,
};

// The provider for the active backend (the build site).
static const CajetaNounProvider* cajeta_xpu_noun_provider(void) {
    int be = cajeta_xpu_active_backend();
    if (be < 0 || be >= CAJ_XPU_COUNT) return NULL;
    return g_xpu_noun_providers[be];
}

// --- AccelerationStructure device-BVH primitives (Part C inc 3b) -------------
// Instance @Native methods on AccelerationStructure.cajeta. The leading `self`
// is the cajeta `this`, ignored — the device side is keyed on the returned
// handle. Ray query / BVH build is a Vulkan-only capability for now; other
// backends return 0 (build) / no-op (free), which the cajeta drop chain handles.

// __cajeta_xpu_accel_build_aabbs(this, aabbs, count) -> int64 handle.
// `aabbs` is a Cajeta float32[] header — { i64 count, [count x f32] data } — so
// the box floats start at offset 8 (matches __cajeta_xpu_texture_upload). Each
// box is 6 floats (minX,minY,minZ,maxX,maxY,maxZ); `count` is the box count.
// STATIC @Native (no `self`): build with an explicit CajetaAsPref override (the
// AccelerationStructure.of factory). `pref` is a CajetaAsPref ordinal.
int64_t __cajeta_xpu_accel_build_aabbs_pref(void* aabbs, uint32_t count,
                                            int32_t pref) {
    if (!aabbs || count == 0) return 0;
    const float* boxes = (const float*) ((const char*) aabbs + 8);
    const CajetaNounProvider* p = cajeta_xpu_noun_provider();
    if (!p || !p->accel_build_aabbs) return 0;  // no device AS on this backend
    CajetaAsImpl impl;                          // reported; recorded via resolve_impl
    int64_t h = p->accel_build_aabbs(boxes, count, pref, &impl);
    (void) impl;
    return h;
}

// INSTANCE @Native (the default ctor): the AUTO preference.
int64_t __cajeta_xpu_accel_build_aabbs(void* self, void* aabbs, uint32_t count) {
    (void) self;
    return __cajeta_xpu_accel_build_aabbs_pref(aabbs, count, CAJ_AS_PREF_AUTO);
}

// __cajeta_xpu_accel_build_triangles(this, vertices, triCount, stride) -> handle.
// `vertices` is a Cajeta float32[] (8-byte count prefix); a triangle soup with
// `stride` floats per vertex (3 = tight). 9 floats define triangle t at vertex
// offset (t*3+v)*stride. v1: software (CPU) path only — Vulkan triangle geometry
// is a follow-up (the Vulkan path still builds AABBs).
int64_t __cajeta_xpu_accel_build_triangles(void* self, void* vertices,
                                           uint32_t triCount, uint32_t stride) {
    (void) self;
    if (!vertices || triCount == 0) return 0;
    const float* verts = (const float*) ((const char*) vertices + 8);
    const CajetaNounProvider* p = cajeta_xpu_noun_provider();
    if (!p || !p->accel_build_triangles) return 0;
    CajetaAsImpl impl;
    int64_t h = p->accel_build_triangles(verts, triCount, stride, &impl);
    (void) impl;
    return h;
}

// STATIC @Native (no `self`): the impl a build with `pref` resolves to on the
// active backend (env > pref > default). The AccelerationStructure.of factory
// records this on the noun; the build uses the same resolver, so the recorded
// impl and the built representation always agree.
int32_t __cajeta_xpu_accel_resolve_impl(int32_t pref) {
    return (int32_t) caj_resolve_as_impl(pref);
}

// INSTANCE @Native (the default ctor records this): the AUTO resolution.
int32_t __cajeta_xpu_accel_impl(void* self) {
    (void) self;
    return (int32_t) caj_resolve_as_impl(CAJ_AS_PREF_AUTO);
}

// Free dispatches on the ACTIVE backend's provider, which branches on the RECORDED
// impl (a forced-software-on-Vulkan AS is a storage buffer, not a host pointer or
// an accel-table entry). v1: an AS is freed while the backend that built it is
// still active (no cross-backend-after-switch free).
void __cajeta_xpu_accel_free(void* self, int64_t handle, int32_t impl) {
    (void) self;
    if (!handle) return;
    const CajetaNounProvider* p = cajeta_xpu_noun_provider();
    if (p && p->accel_free) p->accel_free(handle, (CajetaAsImpl) impl);
}

// Address one axis: clamp-to-edge (addressMode 0) or repeat/wrap (1). `n` > 0.
static inline int cajeta_tex_addr(int c, int n, int32_t addressMode) {
    if (addressMode == 1) {                 // repeat (wrap)
        c %= n;
        if (c < 0) c += n;
        return c;
    }
    if (c < 0) return 0;                     // clamp-to-edge
    if (c >= n) return n - 1;
    return c;
}

// A 4-lane float vector matching LLVM `<4 x float>` in the x86-64 SysV ABI
// (returned in xmm0), so the CPU sampleTexture seam can declare this symbol as
// returning `<4 x float>` and use the result as a Vector<float32,4> directly.
typedef float caj_v4f __attribute__((vector_size(16)));

// Clamp a requested mip level into [0, levels-1].
static inline int cajeta_cpu_lod(const struct cajeta_cpu_texobj* t, int lod) {
    if (lod < 0) return 0;
    if (lod >= t->levels) return t->levels - 1;
    return lod;
}

// Fetch texel (x,y) of mip level `lod` as RGBA from the DECODED float store: read
// `channels` floats from that level's sub-buffer (offset mipoff[lod], width
// mipw[lod]); missing channels default G/B = 0, A = 1. (x,y) are already addressed
// (in-bounds). Level 0 has mipoff 0, so non-mip reads are unchanged.
static inline caj_v4f cajeta_cpu_texel_lod(const struct cajeta_cpu_texobj* t,
                                           int x, int y, int lod) {
    size_t lw = t->mipw[lod];
    const float* p = t->data + t->mipoff[lod] +
                     ((size_t) y * lw + (size_t) x) * t->channels;
    caj_v4f c = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (int i = 0; i < t->channels; ++i) c[i] = p[i];
    return c;
}

// CPU texture sampler — the lowering of `tex.sample(sampler, u, v)` (lod 0) and
// `tex.sampleLod(sampler, u, v, lod)`. (u, v) normalized in [0, 1]; filterMode
// 0 = nearest / 1 = bilinear; addressMode 0 = clamp / 1 = wrap. `lod` selects the
// mip level (CPU v1: nearest mip = floor(lod), clamped; fractional cross-level
// blend is a refinement). The bilinear gather uses the chosen level's dims.
caj_v4f __cajeta_xpu_cpu_tex_sample_rgba(void* texp, int32_t filterMode,
                                         int32_t addressMode, float u, float v,
                                         float lod) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4f zero = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!t || !t->data || t->w == 0 || t->h == 0) return zero;
    int L = cajeta_cpu_lod(t, (int) floorf(lod));
    int W = (int) t->mipw[L], H = (int) t->miph[L];
    if (filterMode == 0) {                   // nearest
        int x = cajeta_tex_addr((int) floorf(u * (float) W), W, addressMode);
        int y = cajeta_tex_addr((int) floorf(v * (float) H), H, addressMode);
        return cajeta_cpu_texel_lod(t, x, y, L);
    }
    // bilinear (texel-center) — blend four RGBA texels of level L
    float fx = u * (float) W - 0.5f;
    float fy = v * (float) H - 0.5f;
    int x0 = (int) floorf(fx), y0 = (int) floorf(fy);
    float dx = fx - (float) x0, dy = fy - (float) y0;
    int cx0 = cajeta_tex_addr(x0,     W, addressMode);
    int cx1 = cajeta_tex_addr(x0 + 1, W, addressMode);
    int cy0 = cajeta_tex_addr(y0,     H, addressMode);
    int cy1 = cajeta_tex_addr(y0 + 1, H, addressMode);
    caj_v4f t00 = cajeta_cpu_texel_lod(t, cx0, cy0, L);
    caj_v4f t10 = cajeta_cpu_texel_lod(t, cx1, cy0, L);
    caj_v4f t01 = cajeta_cpu_texel_lod(t, cx0, cy1, L);
    caj_v4f t11 = cajeta_cpu_texel_lod(t, cx1, cy1, L);
    caj_v4f a = t00 + (t10 - t00) * dx;
    caj_v4f b = t01 + (t11 - t01) * dx;
    return a + (b - a) * dy;
}

// CPU texelFetch — `tex.fetch(x, y)` (lod 0) / `tex.fetchLod(x, y, lod)`: the
// unfiltered, sampler-free read of the exact texel at integer (x, y) in mip level
// `lod`. Coords clamped to the level's dims defensively. G/B = 0, A = 1 for <4 ch.
caj_v4f __cajeta_xpu_cpu_tex_fetch_rgba(void* texp, int32_t x, int32_t y,
                                        int32_t lod) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4f zero = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!t || !t->data || t->w == 0 || t->h == 0) return zero;
    int L = cajeta_cpu_lod(t, lod);
    int W = (int) t->mipw[L], H = (int) t->miph[L];
    int cx = x < 0 ? 0 : (x >= W ? W - 1 : x);
    int cy = y < 0 ? 0 : (y >= H ? H - 1 : y);
    return cajeta_cpu_texel_lod(t, cx, cy, L);
}

// Integer texelFetch — the int twin (raw 32-bit bits read as i32) at mip `lod`.
typedef int32_t caj_v4i __attribute__((vector_size(16)));
caj_v4i __cajeta_xpu_cpu_tex_fetch_rgba_i32(void* texp, int32_t x, int32_t y,
                                            int32_t lod) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4i zero = { 0, 0, 0, 1 };
    if (!t || !t->data || t->w == 0 || t->h == 0) return zero;
    int L = cajeta_cpu_lod(t, lod);
    int W = (int) t->mipw[L], H = (int) t->miph[L];
    int cx = x < 0 ? 0 : (x >= W ? W - 1 : x);
    int cy = y < 0 ? 0 : (y >= H ? H - 1 : y);
    const int32_t* p = (const int32_t*) t->data + t->mipoff[L] +
                       ((size_t) cy * (size_t) W + (size_t) cx) * t->channels;
    caj_v4i c = { 0, 0, 0, 1 };
    for (int i = 0; i < t->channels; ++i) c[i] = p[i];
    return c;
}

// CPU Image2D store/load — the in-process lowering of `img.store(x, y, v)` /
// `img.load(x, y)` (the writable twin of tex.fetch). `imgp` is the host image
// record (a single-channel R32f cajeta_cpu_texobj). Bounds-guarded: an in-range
// store writes data[y*w + x], an out-of-range store is dropped and an OOB load
// returns 0 — so a stray kernel index can't corrupt host memory (the reference
// path is the safe one). LLJIT resolves these like the tex-fetch symbols.
void __cajeta_xpu_cpu_image_store(void* imgp, int32_t x, int32_t y, float v) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) imgp;
    if (!t || !t->data) return;
    if (x < 0 || y < 0 || (uint32_t) x >= t->w || (uint32_t) y >= t->h) return;
    t->data[(size_t) y * t->w + (size_t) x] = v;
}

float __cajeta_xpu_cpu_image_load(void* imgp, int32_t x, int32_t y) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) imgp;
    if (!t || !t->data) return 0.0f;
    if (x < 0 || y < 0 || (uint32_t) x >= t->w || (uint32_t) y >= t->h) return 0.0f;
    return t->data[(size_t) y * t->w + (size_t) x];
}

// --- Texture3D CPU sample/fetch ---------------------------------------------
// 3-D voxel read from the DECODED float volume, row-major (x fastest, then y,
// then z): index = ((z*h + y)*w + x)*channels. The 3-D analogue of
// cajeta_cpu_texel; missing channels default G/B = 0, A = 1.
static inline caj_v4f cajeta_cpu_texel3d(const struct cajeta_cpu_texobj* t,
                                         int x, int y, int z) {
    const float* p = t->data +
        (((size_t) z * t->h + (size_t) y) * t->w + (size_t) x) * t->channels;
    caj_v4f c = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (int i = 0; i < t->channels; ++i) c[i] = p[i];
    return c;
}

// CPU 3-D texture sampler — the lowering of `tex.sample(sampler, u, v, w)`.
// (u, v, w) normalized in [0, 1]; filterMode 0 = nearest / 1 = trilinear;
// addressMode 0 = clamp / 1 = wrap. Trilinear uses the texel-center convention
// (coord = u*N - 0.5) matching GPU texture units, blending the 8 surrounding
// voxels. The 3-D twin of __cajeta_xpu_cpu_tex_sample_rgba.
caj_v4f __cajeta_xpu_cpu_tex3d_sample_rgba(void* texp, int32_t filterMode,
                                           int32_t addressMode, float u, float v,
                                           float w) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4f zero = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!t || !t->data || t->w == 0 || t->h == 0 || t->d == 0) return zero;
    int W = (int) t->w, H = (int) t->h, D = (int) t->d;
    if (filterMode == 0) {                   // nearest
        int x = cajeta_tex_addr((int) floorf(u * (float) W), W, addressMode);
        int y = cajeta_tex_addr((int) floorf(v * (float) H), H, addressMode);
        int z = cajeta_tex_addr((int) floorf(w * (float) D), D, addressMode);
        return cajeta_cpu_texel3d(t, x, y, z);
    }
    // trilinear (texel-center) — blend eight RGBA voxels
    float fx = u * (float) W - 0.5f;
    float fy = v * (float) H - 0.5f;
    float fz = w * (float) D - 0.5f;
    int x0 = (int) floorf(fx), y0 = (int) floorf(fy), z0 = (int) floorf(fz);
    float dx = fx - (float) x0, dy = fy - (float) y0, dz = fz - (float) z0;
    int cx0 = cajeta_tex_addr(x0,     W, addressMode);
    int cx1 = cajeta_tex_addr(x0 + 1, W, addressMode);
    int cy0 = cajeta_tex_addr(y0,     H, addressMode);
    int cy1 = cajeta_tex_addr(y0 + 1, H, addressMode);
    int cz0 = cajeta_tex_addr(z0,     D, addressMode);
    int cz1 = cajeta_tex_addr(z0 + 1, D, addressMode);
    caj_v4f c000 = cajeta_cpu_texel3d(t, cx0, cy0, cz0);
    caj_v4f c100 = cajeta_cpu_texel3d(t, cx1, cy0, cz0);
    caj_v4f c010 = cajeta_cpu_texel3d(t, cx0, cy1, cz0);
    caj_v4f c110 = cajeta_cpu_texel3d(t, cx1, cy1, cz0);
    caj_v4f c001 = cajeta_cpu_texel3d(t, cx0, cy0, cz1);
    caj_v4f c101 = cajeta_cpu_texel3d(t, cx1, cy0, cz1);
    caj_v4f c011 = cajeta_cpu_texel3d(t, cx0, cy1, cz1);
    caj_v4f c111 = cajeta_cpu_texel3d(t, cx1, cy1, cz1);
    // interpolate along x, then y, then z
    caj_v4f a0 = c000 + (c100 - c000) * dx;
    caj_v4f b0 = c010 + (c110 - c010) * dx;
    caj_v4f a1 = c001 + (c101 - c001) * dx;
    caj_v4f b1 = c011 + (c111 - c011) * dx;
    caj_v4f e0 = a0 + (b0 - a0) * dy;
    caj_v4f e1 = a1 + (b1 - a1) * dy;
    return e0 + (e1 - e0) * dz;
}

// CPU 3-D texelFetch — exact voxel at integer (x, y, z), mip 0, unfiltered.
caj_v4f __cajeta_xpu_cpu_tex3d_fetch_rgba(void* texp, int32_t x, int32_t y,
                                          int32_t z) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4f zero = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!t || !t->data || t->w == 0 || t->h == 0 || t->d == 0) return zero;
    int W = (int) t->w, H = (int) t->h, D = (int) t->d;
    int cx = x < 0 ? 0 : (x >= W ? W - 1 : x);
    int cy = y < 0 ? 0 : (y >= H ? H - 1 : y);
    int cz = z < 0 ? 0 : (z >= D ? D - 1 : z);
    return cajeta_cpu_texel3d(t, cx, cy, cz);
}

// CPU 3-D integer texelFetch — the int twin (raw 32-bit voxel bits read as i32).
caj_v4i __cajeta_xpu_cpu_tex3d_fetch_rgba_i32(void* texp, int32_t x, int32_t y,
                                              int32_t z) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4i zero = { 0, 0, 0, 1 };
    if (!t || !t->data || t->w == 0 || t->h == 0 || t->d == 0) return zero;
    int W = (int) t->w, H = (int) t->h, D = (int) t->d;
    int cx = x < 0 ? 0 : (x >= W ? W - 1 : x);
    int cy = y < 0 ? 0 : (y >= H ? H - 1 : y);
    int cz = z < 0 ? 0 : (z >= D ? D - 1 : z);
    const int32_t* p = (const int32_t*) t->data +
        (((size_t) cz * t->h + (size_t) cy) * t->w + (size_t) cx) * t->channels;
    caj_v4i c = { 0, 0, 0, 1 };
    for (int i = 0; i < t->channels; ++i) c[i] = p[i];
    return c;
}

// --- Texture2DArray CPU sample ----------------------------------------------
// A 2-D array stores its `layers` planes exactly like a 3-D volume's z slices
// (index = ((layer*h + y)*w + x)*channels), so `fetch` reuses the 3-D exact-voxel
// read with z = layer. Only `sample` differs: it filters bilinearly WITHIN the
// integer-selected layer (no cross-layer blend — unlike the 3-D trilinear). The
// lowering of `arr.sample(sampler, u, v, layer)`; `layer` is the integer array
// index (clamped), (u, v) normalized.
caj_v4f __cajeta_xpu_cpu_tex2da_sample_rgba(void* texp, int32_t filterMode,
                                            int32_t addressMode, float u, float v,
                                            int32_t layer) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4f zero = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!t || !t->data || t->w == 0 || t->h == 0 || t->d == 0) return zero;
    int W = (int) t->w, H = (int) t->h, D = (int) t->d;
    int z = layer < 0 ? 0 : (layer >= D ? D - 1 : layer);   // clamp layer index
    if (filterMode == 0) {                   // nearest
        int x = cajeta_tex_addr((int) floorf(u * (float) W), W, addressMode);
        int y = cajeta_tex_addr((int) floorf(v * (float) H), H, addressMode);
        return cajeta_cpu_texel3d(t, x, y, z);
    }
    // bilinear (texel-center) within layer z — blend four RGBA texels
    float fx = u * (float) W - 0.5f;
    float fy = v * (float) H - 0.5f;
    int x0 = (int) floorf(fx), y0 = (int) floorf(fy);
    float dx = fx - (float) x0, dy = fy - (float) y0;
    int cx0 = cajeta_tex_addr(x0,     W, addressMode);
    int cx1 = cajeta_tex_addr(x0 + 1, W, addressMode);
    int cy0 = cajeta_tex_addr(y0,     H, addressMode);
    int cy1 = cajeta_tex_addr(y0 + 1, H, addressMode);
    caj_v4f t00 = cajeta_cpu_texel3d(t, cx0, cy0, z);
    caj_v4f t10 = cajeta_cpu_texel3d(t, cx1, cy0, z);
    caj_v4f t01 = cajeta_cpu_texel3d(t, cx0, cy1, z);
    caj_v4f t11 = cajeta_cpu_texel3d(t, cx1, cy1, z);
    caj_v4f a = t00 + (t10 - t00) * dx;
    caj_v4f b = t01 + (t11 - t01) * dx;
    return a + (b - a) * dy;
}

// --- TextureCube CPU sample -------------------------------------------------
// Sample a cube map by a DIRECTION vector. The 6 faces are stored like a 6-layer
// volume (face = the z slice) in the canonical +X,-X,+Y,-Y,+Z,-Z order. This does
// the standard major-axis face projection (matching the GPU cube convention),
// then bilinear within the selected face. The lowering of
// `cube.sample(sampler, x, y, z)`; the direction need not be normalized.
caj_v4f __cajeta_xpu_cpu_texcube_sample_rgba(void* texp, int32_t filterMode,
                                             int32_t addressMode, float x, float y,
                                             float z) {
    struct cajeta_cpu_texobj* t = (struct cajeta_cpu_texobj*) texp;
    caj_v4f zero = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (!t || !t->data || t->w == 0 || t->h == 0 || t->d < 6) return zero;
    float ax = fabsf(x), ay = fabsf(y), az = fabsf(z);
    int face; float sc, tc, ma;
    if (ax >= ay && ax >= az) {            // major axis X
        ma = ax;
        if (x >= 0.0f) { face = 0; sc = -z; tc = -y; }   // +X
        else           { face = 1; sc =  z; tc = -y; }   // -X
    } else if (ay >= ax && ay >= az) {     // major axis Y
        ma = ay;
        if (y >= 0.0f) { face = 2; sc =  x; tc =  z; }   // +Y
        else           { face = 3; sc =  x; tc = -z; }   // -Y
    } else {                               // major axis Z
        ma = az;
        if (z >= 0.0f) { face = 4; sc =  x; tc = -y; }   // +Z
        else           { face = 5; sc = -x; tc = -y; }   // -Z
    }
    if (ma == 0.0f) ma = 1.0f;             // degenerate (0,0,0) → face 0 center
    float u = 0.5f * (sc / ma + 1.0f);
    float v = 0.5f * (tc / ma + 1.0f);
    int W = (int) t->w, H = (int) t->h;
    if (filterMode == 0) {                 // nearest
        int xi = cajeta_tex_addr((int) floorf(u * (float) W), W, addressMode);
        int yi = cajeta_tex_addr((int) floorf(v * (float) H), H, addressMode);
        return cajeta_cpu_texel3d(t, xi, yi, face);
    }
    // bilinear (texel-center) within the selected face
    float fx = u * (float) W - 0.5f;
    float fy = v * (float) H - 0.5f;
    int x0 = (int) floorf(fx), y0 = (int) floorf(fy);
    float dx = fx - (float) x0, dy = fy - (float) y0;
    int cx0 = cajeta_tex_addr(x0,     W, addressMode);
    int cx1 = cajeta_tex_addr(x0 + 1, W, addressMode);
    int cy0 = cajeta_tex_addr(y0,     H, addressMode);
    int cy1 = cajeta_tex_addr(y0 + 1, H, addressMode);
    caj_v4f c00 = cajeta_cpu_texel3d(t, cx0, cy0, face);
    caj_v4f c10 = cajeta_cpu_texel3d(t, cx1, cy0, face);
    caj_v4f c01 = cajeta_cpu_texel3d(t, cx0, cy1, face);
    caj_v4f c11 = cajeta_cpu_texel3d(t, cx1, cy1, face);
    caj_v4f a = c00 + (c10 - c00) * dx;
    caj_v4f b = c01 + (c11 - c01) * dx;
    return a + (b - a) * dy;
}

// --- Launch + module registration -------------------------------------------
// The compiler lowers `kernel.launch(stream, grid:, block:)(args)` to a call
// here, passing the kernel's PTX entry name, 1-D grid/block, and the CUDA
// kernelParams argv (an array of pointers to each argument value). The real
// NVPTX path (cuLaunchKernel via the dlopen'd driver) lands in the host-launch
// runtime; this is the not-yet-wired no-op so the symbol resolves and host
// codegen of a launch site links.
// CUDA launch: lazily load the module + resolve the function, then 1-D launch.
static void cajeta_xpu_launch_cuda(const char* kernelName,
                                   int32_t gridX, int32_t gridY, int32_t gridZ,
                                   int32_t blockX, int32_t blockY, int32_t blockZ,
                                   uint32_t sharedBytes, void* argv,
                                   int64_t streamHandle) {
    pthread_mutex_lock(&g_xpu_cuda_lock);
    struct cajeta_xpu_module* e = cajeta_xpu_find_module(kernelName);
    if (e) {
        if (!e->module) {
            if (g_xpu_cuda.cuModuleLoadData(&e->module, e->image) != 0)
                e->module = NULL;
        }
        if (e->module && !e->function) {
            if (g_xpu_cuda.cuModuleGetFunction(&e->function, e->module,
                                               kernelName) != 0)
                e->function = NULL;
        }
    }
    void* fn = e ? e->function : NULL;
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    if (!fn) {
        fprintf(stderr, "cajeta.xpu: no registered kernel '%s' to launch\n",
                kernelName);
        return;
    }
    // Image2D safety guard: the CUDA surface RUNTIME (cuSurfObjectCreate +
    // per-launch surface marshalling) is not wired yet — __cajeta_xpu_image_alloc
    // returns 0 for CUDA until B5. If a kernel has a storage-image param with an
    // unbacked (0) handle, dispatching would feed sust/suld a null surface and
    // fault; skip the launch instead (mirrors the HIP launchOk=0 guard), so the
    // image stays unwritten and the caller degrades cleanly. (The compiler-side
    // NVPTX storeImage/loadImage lowering is proven via the PTX emit test; the
    // device run lands when the CUDA surface runtime does — B5.)
    {
        void** av = (void**) argv;
        struct cajeta_kparams* kp = cajeta_xpu_find_kparams(kernelName);
        if (kp && kp->count > 0 && av) {
            for (int i = 0; i < kp->count; ++i)
                if (kp->kind[i] == CAJETA_KP_IMAGE && av[i] &&
                    *(int64_t*) av[i] == 0) {
                    fprintf(stderr, "cajeta.xpu: CUDA storage images need the "
                            "surface runtime (pending B5); not launching '%s'\n",
                            kernelName);
                    return;
                }
        }
    }
    // H9: the CUDA context is bound to the thread that created it (cuCtxCreate);
    // a launch from a different thread (the carrier fiber vs the main thread) runs
    // with no current context -> CUDA_ERROR_INVALID_CONTEXT and the launch is a
    // silent no-op. Make the context current on this thread first, and surface a
    // launch failure instead of discarding the return code.
    if (g_xpu_cuda.cuCtxSetCurrent) g_xpu_cuda.cuCtxSetCurrent(g_xpu_cuda.ctx);
    // 3-D grid/block; default stream; kernelParams = the CUDA argv the launch
    // site marshalled (pointers to each arg value). sharedBytes sizes the
    // kernel's dynamic (extern) shared memory; 0 for static-only kernels.
    int launchRc = g_xpu_cuda.cuLaunchKernel(
        fn, (unsigned) gridX, (unsigned) gridY, (unsigned) gridZ,
        (unsigned) blockX, (unsigned) blockY, (unsigned) blockZ,
        (unsigned) sharedBytes, /*stream=*/(void*) (intptr_t) streamHandle,
        (void**) argv, /*extra=*/NULL);
    if (launchRc != 0)
        fprintf(stderr, "cajeta.xpu: cuLaunchKernel('%s') failed (%d)\n",
                kernelName, launchRc);
}

// HIP launch: lazily load the hsaco module + resolve the function (reusing the
// shared module table — only one device backend is active per run), then 1-D
// launch. Mirrors cajeta_xpu_launch_cuda with hip* entry points. Texture params
// (Item 8 Stage C) are translated here: the argv slot holds a texture-record
// handle, so a hipTextureObject is built from its hipArray + the paired Sampler's
// modes and substituted into the kernelParams (the kernel reads the image+sampler
// SRDs from that object via __ockl_image_sample_2D); destroyed after the launch.
static void cajeta_xpu_launch_hip(const char* kernelName,
                                  int32_t gridX, int32_t gridY, int32_t gridZ,
                                  int32_t blockX, int32_t blockY, int32_t blockZ,
                                  uint32_t sharedBytes, void* argvv,
                                  int64_t streamHandle) {
    pthread_mutex_lock(&g_xpu_cuda_lock);
    struct cajeta_xpu_module* e = cajeta_xpu_find_module(kernelName);
    if (e) {
        if (!e->module) {
            if (g_xpu_hip.hipModuleLoadData(&e->module, e->image) != 0)
                e->module = NULL;
        }
        if (e->module && !e->function) {
            if (g_xpu_hip.hipModuleGetFunction(&e->function, e->module,
                                               kernelName) != 0)
                e->function = NULL;
        }
    }
    void* fn = e ? e->function : NULL;
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    if (!fn) {
        fprintf(stderr, "cajeta.xpu: no registered kernel '%s' to launch\n",
                kernelName);
        return;
    }
    void** argv = (void**) argvv;

    // Texture/surface-object translation (only if this kernel has a Texture2D or
    // Image2D param). A Texture2D param is bound as a sampled texture object; an
    // Image2D param as a writable surface object — both arrive at the kernel as a
    // ptr-addrspace(4) kernarg, so we substitute &objVal into the argv slot.
    void** useArgv = argv;
    void* subArgv[64];
    void* texObjVals[8];
    int64_t texObjs[8];
    int ntex = 0;
    void* surfObjVals[8];
    int64_t surfObjs[8];
    int nsurf = 0;
    void* bufArrVals[8];   // bindless device-array pointer values (&slot stays stable)
    void* bufArrDev[8];    // device copies of [count, h…] to free after the launch
    int nbufarr = 0;
    int launchOk = 1;
    struct cajeta_kparams* kp = cajeta_xpu_find_kparams(kernelName);
    if (kp && kp->count > 0 && kp->count <= 64) {
        int hasTex = 0, hasImg = 0, hasBufArr = 0;
        for (int i = 0; i < kp->count; ++i) {
            if (kp->kind[i] == CAJETA_KP_TEXTURE) hasTex = 1;
            else if (kp->kind[i] == CAJETA_KP_IMAGE) hasImg = 1;
            else if (kp->kind[i] == CAJETA_KP_BUFFER_ARRAY) hasBufArr = 1;
        }
        if (hasTex || hasImg || hasBufArr) {
            // The (single, v1) Sampler param supplies the filter/address modes.
            int32_t filterMode = CAJ_HIP_FILTER_LINEAR, addressMode = 0;
            for (int i = 0; i < kp->count; ++i)
                if (kp->kind[i] == CAJETA_KP_SAMPLER) {
                    const int32_t* modes = (const int32_t*) argv[i];
                    filterMode = modes[0]; addressMode = modes[1];
                    break;
                }
            for (int i = 0; i < kp->count; ++i) {
                subArgv[i] = argv[i];
                if (kp->kind[i] == CAJETA_KP_TEXTURE) {
                    if (ntex >= 8) {   // M6: more textures than the texObj buffers
                        fprintf(stderr, "cajeta.xpu: HIP kernel '%s' uses more than "
                                "8 textures (unsupported); not launching\n", kernelName);
                        launchOk = 0; break;
                    }
                    int64_t rec = *(int64_t*) argv[i];   // texture-record handle
                    int64_t obj = cajeta_xpu_hip_make_texobj(rec, filterMode,
                                                             addressMode);
                    if (!obj) {        // M5: texture-object creation failed
                        fprintf(stderr, "cajeta.xpu: HIP texture-object creation "
                                "failed for kernel '%s'; not launching\n", kernelName);
                        launchOk = 0; break;
                    }
                    texObjs[ntex] = obj;
                    texObjVals[ntex] = (void*) (intptr_t) obj;
                    subArgv[i] = &texObjVals[ntex];      // arg = the texObj ptr
                    ++ntex;
                } else if (kp->kind[i] == CAJETA_KP_IMAGE) {
                    if (nsurf >= 8) {  // more storage images than the surfObj buffers
                        fprintf(stderr, "cajeta.xpu: HIP kernel '%s' uses more than "
                                "8 storage images (unsupported); not launching\n",
                                kernelName);
                        launchOk = 0; break;
                    }
                    int64_t rec = *(int64_t*) argv[i];   // image-record handle
                    int64_t obj = cajeta_xpu_hip_make_surfobj(rec);
                    if (!obj) {        // surface-object creation failed/unsupported
                        fprintf(stderr, "cajeta.xpu: HIP surface-object creation "
                                "failed for kernel '%s'; not launching\n", kernelName);
                        launchOk = 0; break;
                    }
                    surfObjs[nsurf] = obj;
                    surfObjVals[nsurf] = (void*) (intptr_t) obj;
                    subArgv[i] = &surfObjVals[nsurf];    // arg = the surfObj ptr
                    ++nsurf;
                } else if (kp->kind[i] == CAJETA_KP_BUFFER_ARRAY) {
                    // Bindless Buffer<T>[]: argv[i] points at the HOST-marshalled
                    // [i64 count, i64 h0 … ] handle array. The device kernel takes a
                    // global pointer to it (the default bufferArrayElement flat-loads
                    // each handle, which is itself a device address). Copy the array
                    // into device memory and pass &devPtr as the kernarg.
                    if (nbufarr >= 8) {
                        fprintf(stderr, "cajeta.xpu: HIP kernel '%s' uses more than "
                                "8 bindless buffer arrays (unsupported); not "
                                "launching\n", kernelName);
                        launchOk = 0; break;
                    }
                    const int64_t* hostArr = (const int64_t*) argv[i];
                    int64_t cnt = hostArr ? hostArr[0] : -1;
                    if (cnt < 0 || cnt > 16) {   // 16 = kMaxBindlessBuffers (host cap)
                        fprintf(stderr, "cajeta.xpu: HIP kernel '%s' bindless buffer-"
                                "array count %lld out of range; not launching\n",
                                kernelName, (long long) cnt);
                        launchOk = 0; break;
                    }
                    size_t bytes = (size_t) (cnt + 1) * sizeof(int64_t);
                    void* dev = NULL;
                    if (g_xpu_hip.hipMalloc(&dev, bytes) != 0 || !dev) {
                        fprintf(stderr, "cajeta.xpu: HIP bindless buffer-array device "
                                "alloc failed for kernel '%s'; not launching\n",
                                kernelName);
                        launchOk = 0; break;
                    }
                    if (g_xpu_hip.hipMemcpyHtoD(dev, hostArr, bytes) != 0) {
                        g_xpu_hip.hipFree(dev);
                        fprintf(stderr, "cajeta.xpu: HIP bindless buffer-array upload "
                                "failed for kernel '%s'; not launching\n", kernelName);
                        launchOk = 0; break;
                    }
                    bufArrDev[nbufarr] = dev;
                    bufArrVals[nbufarr] = dev;           // the device-array address
                    subArgv[i] = &bufArrVals[nbufarr];   // kernarg = &devPtr
                    ++nbufarr;
                }
            }
            useArgv = subArgv;
        }
    }

    if (launchOk)
        g_xpu_hip.hipModuleLaunchKernel(fn, (unsigned) gridX, (unsigned) gridY,
                                        (unsigned) gridZ, (unsigned) blockX,
                                        (unsigned) blockY, (unsigned) blockZ,
                                        (unsigned) sharedBytes,
                                        /*stream=*/(void*) (intptr_t) streamHandle,
                                        useArgv, /*extra=*/NULL);
    if (ntex > 0 || nsurf > 0 || nbufarr > 0) {
        if (launchOk)
            g_xpu_hip.hipDeviceSynchronize();   // finish before freeing resources
        for (int i = 0; i < ntex; ++i)          // also frees objs made before a skip
            if (texObjs[i] && g_xpu_hip.hipDestroyTextureObject)
                g_xpu_hip.hipDestroyTextureObject((void*) (intptr_t) texObjs[i]);
        for (int i = 0; i < nsurf; ++i)
            if (surfObjs[i] && g_xpu_hip.hipDestroySurfaceObject)
                g_xpu_hip.hipDestroySurfaceObject((void*) (intptr_t) surfObjs[i]);
        for (int i = 0; i < nbufarr; ++i)       // free the device handle-array copies
            if (bufArrDev[i] && g_xpu_hip.hipFree)
                g_xpu_hip.hipFree(bufArrDev[i]);
    }
}

// Vulkan launch: translate the uniform kernelParams argv into descriptor
// bindings using the per-kernel param metadata — buffer args map to their
// existing storage buffers (argv slot holds the buffer-table handle), scalar
// args are copied into transient single-element SSBOs (freed after) — then
// dispatch gridX work-groups (the local size is baked into the SPIR-V). This is
// the one backend whose launch ABI forks from the pointer-arg kernelParams
// model: Vulkan's compute entry has no params, only descriptor bindings.
static void cajeta_xpu_launch_vulkan(const char* kernelName,
                                     int32_t gridX, int32_t gridY, int32_t gridZ,
                                     int32_t blockX, int32_t blockY, int32_t blockZ,
                                     int32_t sharedBytes, void* argvv) {
    void** argv = (void**) argvv;
    // kparams are shared across variants (looked up by the base name); the launch
    // resolves the AS bind kind from the recorded impl below.
    struct cajeta_kparams* kp = cajeta_xpu_find_kparams(kernelName);
    if (!kp || kp->count <= 0 || kp->count > 64) {
        fprintf(stderr,
                "cajeta.xpu: missing/invalid parameter metadata for Vulkan "
                "kernel '%s'\n", kernelName);
        return;
    }
    const int n = kp->count;

    // Variant selection (inc-4 brick #3): if an AccelerationStructure argument was
    // built as a software BVH, launch the "<name>$sw" variant — the SoftwareRayQuery
    // walk in plain SPIR-V, AS bound as a storage buffer — instead of the native
    // module. The impl is recorded at the AS POD's offset 12 ({i64 handle, u32
    // count, i32 impl}). v1: AS args in one launch share one impl.
    int asSoftware = 0;
    for (int i = 0; i < n; ++i) {
        if (kp->kind[i] == CAJETA_KP_ACCEL &&
            ((const int32_t*) argv[i])[3] == CAJ_AS_IMPL_SOFTWARE_BVH) {
            asSoftware = 1;
            break;
        }
    }
    char variantName[128];
    const char* launchName = kernelName;
    if (asSoftware) {
        snprintf(variantName, sizeof(variantName), "%s$sw", kernelName);
        launchName = variantName;
    }

    pthread_mutex_lock(&g_xpu_cuda_lock);
    struct cajeta_xpu_module* e = cajeta_xpu_find_module(launchName);
    const void* spirv = e ? e->image : NULL;
    uint64_t len = e ? e->len : 0;
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    if (!spirv || len < 4) {
        fprintf(stderr,
                "cajeta.xpu: no registered SPIR-V kernel '%s' to launch\n",
                launchName);
        return;
    }
    int64_t bindings[64];
    uint8_t bkinds[64];                     // per-binding resource kind
    int64_t transient[64];                  // transient scalar SSBOs to free
    int64_t samplers[64];                   // transient VkSamplers (as int64)
    int ntrans = 0, nsamp = 0;
    int built = 1;
    for (int i = 0; i < n; ++i) {
        switch (kp->kind[i]) {
            case CAJETA_KP_BUFFER:
                bindings[i] = *(int64_t*) argv[i];    // existing storage buffer
                bkinds[i] = CAJ_VKB_BUFFER;
                break;
            case CAJETA_KP_BUFFER_ARRAY:
                // argv[i] points at the marshalled [int64 count, int64 h0 …]
                // handle array; pass that pointer through to the descriptor-array
                // write (which reads the count + handles).
                bindings[i] = (int64_t) (intptr_t) argv[i];
                bkinds[i] = CAJ_VKB_BUFFER_ARRAY;
                break;
            case CAJETA_KP_TEXTURE:
                // argv slot holds the Texture2D deviceHandle = texture-table index.
                bindings[i] = *(int64_t*) argv[i];
                bkinds[i] = CAJ_VKB_TEXTURE;
                break;
            case CAJETA_KP_IMAGE:
                // argv slot holds the Image2D deviceHandle = texture-table index
                // (storage image). Bind it as a STORAGE_IMAGE (GENERAL layout).
                bindings[i] = *(int64_t*) argv[i];
                bkinds[i] = CAJ_VKB_STORAGE_IMAGE;
                break;
            case CAJETA_KP_ACCEL: {
                // argv slot points at the AccelerationStructure POD:
                // { i64 deviceHandle, u32 primitiveCount, i32 impl }. The first
                // field is the handle; the bind kind follows the noun's RECORDED
                // impl (the matching verb variant was already selected above):
                // native BLAS → an acceleration-structure descriptor; software
                // BVH → the storage buffer the "$sw" variant reads as bvh[i].
                int32_t asImpl = ((const int32_t*) argv[i])[3];
                bindings[i] = *(int64_t*) argv[i];
                bkinds[i] = (asImpl == CAJ_AS_IMPL_SOFTWARE_BVH) ? CAJ_VKB_BUFFER
                                                                 : CAJ_VKB_ACCEL;
                break;
            }
            case CAJETA_KP_SAMPLER: {
                // argv slot points at the by-value Sampler POD: { i32 filterMode,
                // i32 addressMode }. Build a transient VkSampler from it.
                const int32_t* modes = (const int32_t*) argv[i];
                int64_t s = cajeta_xpu_vk_make_sampler(modes[0], modes[1]);
                if (!s) { built = 0; break; }
                bindings[i] = s;
                bkinds[i] = CAJ_VKB_SAMPLER;
                samplers[nsamp++] = s;
                break;
            }
            default: {   // scalar by value -> transient single-element SSBO
                uint32_t sz = kp->byteSize[i] ? kp->byteSize[i] : 4u;
                int64_t h = cajeta_xpu_vk_alloc(sz);
                if (!h) { built = 0; break; }
                void* m = cajeta_xpu_vk_mapped(h);
                if (m) memcpy(m, argv[i], sz);
                bindings[i] = h;
                bkinds[i] = CAJ_VKB_BUFFER;
                transient[ntrans++] = h;
                break;
            }
        }
        if (!built) break;
    }
    if (built)
        cajeta_xpu_vk_launch(spirv, len, launchName, bindings, bkinds, n,
                             (unsigned) gridX, (unsigned) gridY, (unsigned) gridZ,
                             (unsigned) (blockX > 0 ? blockX : 1),
                             (unsigned) (blockY > 0 ? blockY : 1),
                             (unsigned) (blockZ > 0 ? blockZ : 1),
                             (unsigned) (sharedBytes > 0 ? sharedBytes : 0));
    for (int i = 0; i < ntrans; ++i) cajeta_xpu_vk_free(transient[i]);
    for (int i = 0; i < nsamp; ++i) cajeta_xpu_vk_destroy_sampler(samplers[i]);
}

// The host-source `kernel.launch(...)` entry point: dispatch to the active
// backend (chosen + cached on first device touch).
void __cajeta_xpu_launch(const char* kernelName,
                         int32_t gridX, int32_t gridY, int32_t gridZ,
                         int32_t blockX, int32_t blockY, int32_t blockZ,
                         uint32_t sharedBytes, void* argv, int64_t streamHandle) {
    if (!kernelName) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            // streamHandle (0 = default stream) orders this launch with the
            // async copies queued on the same stream.
            cajeta_xpu_launch_cuda(kernelName, gridX, gridY, gridZ,
                                   blockX, blockY, blockZ, sharedBytes, argv,
                                   streamHandle);
            return;
        case CAJ_XPU_HIP:
            cajeta_xpu_launch_hip(kernelName, gridX, gridY, gridZ,
                                  blockX, blockY, blockZ, sharedBytes, argv,
                                  streamHandle);
            return;
        case CAJ_XPU_VULKAN:
            // Vulkan v1 submits on its own queue; per-stream ordering is a
            // follow-on (cajeta-xpu). The stream handle is accepted, not used.
            (void) streamHandle;
            cajeta_xpu_launch_vulkan(kernelName, gridX, gridY, gridZ,
                                     blockX, blockY, blockZ,
                                     (int32_t) sharedBytes, argv);
            return;
        case CAJ_XPU_CPU:
            // CPU launches run synchronously; the stream is ordering-irrelevant.
            (void) streamHandle;
            cajeta_xpu_launch_cpu(kernelName, gridX, gridY, gridZ,
                                  blockX, blockY, blockZ,
                                  (int32_t) sharedBytes, argv);
            return;
        default: return;   // none: diagnostic emitted
    }
}

// Register a kernel's compiled cubin image under its PTX entry name. The
// device-cubin pass emits a module global constructor that calls this; the
// launch path (above) loads the CUDA module + resolves the function lazily on
// first use. The image pointer lives in the host module's constant data and
// stays valid for the process lifetime.
void __cajeta_xpu_register_module(const char* kernelName, const void* image,
                                  uint64_t len) {
    if (!kernelName || !image) return;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    if (!cajeta_xpu_find_module(kernelName)
            && g_xpu_module_count < CAJETA_XPU_MAX_MODULES) {
        struct cajeta_xpu_module* e = &g_xpu_modules[g_xpu_module_count++];
        strncpy(e->name, kernelName, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        e->image = image;
        e->len = len;
        e->module = NULL;
        e->function = NULL;
    }
    pthread_mutex_unlock(&g_xpu_cuda_lock);
}

// ---------------------------------------------------------------------------
// cajeta.net — NET-1.1 native socket intrinsics (BSD sockets / Winsock).
//
// Kept in its own reviewable source file and #included here so it rides the
// single-TU runtime → bitcode → embed build without a CMake change (the build
// compiles ONLY cajeta_runtime.c to bitcode; sibling .c files must be textually
// included to be embedded + linker-merged into user modules). See the file
// header in cajeta_net_socket.c for the full ABI + errno-shim rationale.
// ---------------------------------------------------------------------------
#include "cajeta_net_socket.c"

// ---------------------------------------------------------------------------
// cajeta.net — NET-1.7 non-blocking mode + WouldBlock-as-a-value intrinsics.
// MUST be included AFTER cajeta_net_socket.c (reuses its fd-ABI helpers,
// cajeta_net_raw_errno, and the CAJETA_NET_* ordinal contract).
// ---------------------------------------------------------------------------
#include "cajeta_net_nonblocking.c"

// ---------------------------------------------------------------------------
// cajeta.net — NET-1.2 native sockaddr marshalling intrinsics.
// ---------------------------------------------------------------------------
#include "cajeta_net_sockaddr.c"

// ---------------------------------------------------------------------------
// cajeta.net — NET-1.3 native getsockname/getpeername intrinsics.
// MUST be included AFTER cajeta_net_socket.c (reuses its fd-ABI typedefs +
// cajeta_net_from_fd helper).
// ---------------------------------------------------------------------------
#include "cajeta_net_getname.c"

// ---------------------------------------------------------------------------
// cajeta.net — NET-1.4 native TcpListener support intrinsics.
// MUST be included AFTER cajeta_net_socket.c + cajeta_net_sockaddr.c (reuses
// their fd narrowing helpers, SOL_SOCKET context, and storage-size helper).
// ---------------------------------------------------------------------------
#include "cajeta_net_listener.c"

// ---------------------------------------------------------------------------
// cajeta.net — NET-1.6 native typed socket-option surface. MUST be included
// AFTER cajeta_net_socket.c (reuses its fd-ABI helpers).
// ---------------------------------------------------------------------------
#include "cajeta_net_socket_options.c"

// ---------------------------------------------------------------------------
// cajeta.net — NET-2.1 native getaddrinfo (name resolution) intrinsics.
// MUST be included AFTER cajeta_net_socket.c (uses its file-static
// cajeta_net_ensure_init() so WSAStartup ran on Windows).
// ---------------------------------------------------------------------------
#include "cajeta_net_getaddrinfo.c"

// ---------------------------------------------------------------------------
// cajeta.net.reactor — NET-3.1 reactor engine intrinsics (init/register/
// deregister/await_readable/await_writable + portable select() probe).
// MUST be included AFTER cajeta_net_socket.c (reuses its file-static
// cajeta_net_from_fd / cajeta_net_ensure_init) AND after the R9.4 reactor
// block that defines __cajeta_io_wait (which it delegates to on Linux).
// ---------------------------------------------------------------------------
#include "cajeta_net_reactor.c"

// ---------------------------------------------------------------------------
// cajeta.net.reactor — NET-3.2 reactor lifecycle (lazy init / clean shutdown).
// MUST be included AFTER cajeta_net_reactor.c: its shutdown drains NET-3.1's
// live-registration counter (__cajeta_net_reactor_active_reset) and its init is
// the body NET-3.1's __cajeta_net_reactor_init delegates to. The runtime
// teardown path (__cajeta_task_shutdown, far above) forward-declares + calls
// __cajeta_net_reactor_shutdown from here.
// ---------------------------------------------------------------------------
#include "cajeta_net_reactor_lifecycle.c"

// ---------------------------------------------------------------------------
// cajeta.hash — NET-11.2 SHA-1 (FIPS 180-4), WebSocket handshake only.
//
// Kept in its own reviewable source file and #included here so it rides the
// single-TU runtime -> bitcode -> embed build without a CMake change (the build
// compiles ONLY cajeta_runtime.c to bitcode; sibling .c files must be textually
// included to be embedded + linker-merged into user modules). See the file
// header in cajeta_sha1.c for the not-for-security-use rationale.
// ---------------------------------------------------------------------------
#include "cajeta_sha1.c"
