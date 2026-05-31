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

#define CAJETA_FIBER_STACK_SIZE (64 * 1024)

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
};

static pthread_mutex_t __cajeta_task_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  __cajeta_task_queue_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  __cajeta_task_done_cond  = PTHREAD_COND_INITIALIZER;
// Ready queue: fibers whose next scheduler tick will resume execution.
static struct cajeta_fiber* __cajeta_ready_head = NULL;
static struct cajeta_fiber* __cajeta_ready_tail = NULL;
// Parked list: fibers blocked inside __cajeta_task_wait waiting for some
// task's done flag to flip. Wake-all-on-any-complete is the v1 strategy.
static struct cajeta_fiber* __cajeta_parked_head = NULL;
static int __cajeta_task_workers_started = 0;
static pthread_t __cajeta_task_worker;
// Set by __cajeta_task_shutdown to signal the carrier loop to exit.
// The carrier's pthread_cond_wait predicate checks this alongside
// the ready-queue head so a shutdown request reliably unblocks it.
static int __cajeta_task_shutdown_requested = 0;

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

// Fiber entry trampoline — invoked by makecontext on first resume. Runs
// the user trampoline (which calls the async fn + signals done) and then
// falls through to uc_link, which returns to the carrier.
static void __cajeta_fiber_entry(void) {
    struct cajeta_fiber* f = __cajeta_current_fiber;
    f->trampoline(f->trampoline_arg);
    f->state = CAJETA_FIBER_DONE;
    // Falling off the end here triggers uc_link -> back to carrier.
}

// Move every parked fiber to the ready queue. Caller holds the mutex.
// Called from __cajeta_task_complete — the cheapest correct policy when
// we don't track who-awaits-whom yet. Each woken parker will spin-check
// its own done flag and (likely) re-park if it wasn't the target.
static void __cajeta_wake_all_parked_locked(void) {
    while (__cajeta_parked_head) {
        struct cajeta_fiber* f = __cajeta_parked_head;
        __cajeta_parked_head = f->next;
        f->state = CAJETA_FIBER_READY;
        f->next = NULL;
        if (__cajeta_ready_tail) {
            __cajeta_ready_tail->next = f;
            __cajeta_ready_tail = f;
        } else {
            __cajeta_ready_head = f;
            __cajeta_ready_tail = f;
        }
    }
    pthread_cond_broadcast(&__cajeta_task_queue_cond);
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

// Carrier loop: pop a ready fiber, swap into it, observe its post-yield
// state, free if done. Single carrier suffices for v1 because all fibers
// are cooperative — they yield to each other inside this thread.
static void* __cajeta_carrier_loop(void* arg) {
    (void) arg;
    for (;;) {
        pthread_mutex_lock(&__cajeta_task_mutex);
        while (!__cajeta_ready_head && !__cajeta_task_shutdown_requested) {
            pthread_cond_wait(&__cajeta_task_queue_cond, &__cajeta_task_mutex);
        }
        if (__cajeta_task_shutdown_requested && !__cajeta_ready_head) {
            pthread_mutex_unlock(&__cajeta_task_mutex);
            return NULL;
        }
        struct cajeta_fiber* f = __cajeta_ready_head;
        __cajeta_ready_head = f->next;
        if (!__cajeta_ready_head) __cajeta_ready_tail = NULL;
        f->next = NULL;
        pthread_mutex_unlock(&__cajeta_task_mutex);

        __cajeta_current_fiber = f;
        f->state = CAJETA_FIBER_RUNNING;
        if (!f->stack) {
            // First-resume init: allocate the stack and prime the context
            // to dispatch to __cajeta_fiber_entry. uc_link points back at
            // the carrier so a normal fall-off-the-end returns here.
            f->stack = malloc(CAJETA_FIBER_STACK_SIZE);
            if (!f->stack) {
                fprintf(stderr, "cajeta: fiber stack malloc failed\n");
                abort();
            }
            __cajeta_getcontext(&f->ctx);
            f->ctx.uc_stack.ss_sp = f->stack;
            f->ctx.uc_stack.ss_size = CAJETA_FIBER_STACK_SIZE;
            f->ctx.uc_link = &__cajeta_carrier_ctx;
            __cajeta_makecontext(&f->ctx, __cajeta_fiber_entry, 0);
        }
        __cajeta_swapcontext(&__cajeta_carrier_ctx, &f->ctx);
        __cajeta_current_fiber = NULL;
        if (f->state == CAJETA_FIBER_DONE) {
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
    pthread_mutex_unlock(&__cajeta_task_mutex);
    pthread_join(__cajeta_task_worker, NULL);
    // Reset state so a subsequent __cajeta_task_run (e.g. the next test's
    // first spawn) starts a fresh carrier with a clean queue.
    pthread_mutex_lock(&__cajeta_task_mutex);
    __cajeta_task_shutdown_requested = 0;
    __cajeta_task_workers_started = 0;
    pthread_mutex_unlock(&__cajeta_task_mutex);
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
    if (fiber_slot) *fiber_slot = f;

    pthread_mutex_lock(&__cajeta_task_mutex);
    if (!__cajeta_task_workers_started) {
        __cajeta_task_workers_started = 1;
        pthread_create(&__cajeta_task_worker, NULL,
                       __cajeta_carrier_loop, NULL);
    }
    if (__cajeta_ready_tail) {
        __cajeta_ready_tail->next = f;
        __cajeta_ready_tail = f;
    } else {
        __cajeta_ready_head = f;
        __cajeta_ready_tail = f;
    }
    pthread_cond_signal(&__cajeta_task_queue_cond);
    pthread_mutex_unlock(&__cajeta_task_mutex);
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
        while (!*done_addr) {
            __cajeta_fiber_park();
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
    pthread_mutex_lock(&__cajeta_task_mutex);
    *done_addr = 1;
    pthread_cond_broadcast(&__cajeta_task_done_cond);
    __cajeta_wake_all_parked_locked();
    pthread_mutex_unlock(&__cajeta_task_mutex);
}

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
            for (int j = i + 1; j < f->count; j++) {
                if (f->entries[j].fiber_slot && *f->entries[j].fiber_slot) {
                    __cajeta_fiber_cancel(
                        (struct cajeta_fiber*) *f->entries[j].fiber_slot,
                        trigger);
                }
            }
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
                for (int j = i + 1; j < f->count; j++) {
                    if (f->entries[j].fiber_slot && *f->entries[j].fiber_slot) {
                        __cajeta_fiber_cancel(
                            (struct cajeta_fiber*) *f->entries[j].fiber_slot,
                            frame_trigger);
                    }
                }
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
        if (!l->held) {
            l->held = 1;
            pthread_mutex_unlock(&l->mutex);
            return;
        }
        struct cajeta_fiber* self = __cajeta_current_fiber;
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
        // Move the woken fiber onto the carrier's ready queue. Done
        // under the task mutex so it pairs correctly with the carrier's
        // dequeue.
        pthread_mutex_lock(&__cajeta_task_mutex);
        next->state = CAJETA_FIBER_READY;
        next->next = NULL;
        if (__cajeta_ready_tail) {
            __cajeta_ready_tail->next = next;
            __cajeta_ready_tail = next;
        } else {
            __cajeta_ready_head = next;
            __cajeta_ready_tail = next;
        }
        pthread_cond_signal(&__cajeta_task_queue_cond);
        pthread_mutex_unlock(&__cajeta_task_mutex);
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
    pthread_cond_destroy(&l->released_cond);
    pthread_mutex_destroy(&l->mutex);
    free(l);
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
static pthread_mutex_t __cajeta_trace_mutex = PTHREAD_MUTEX_INITIALIZER;

#define CAJETA_TRACE_MAX_FRAMES 64

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
    e->next = __cajeta_trace_table;
    __cajeta_trace_table = e;
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
    void* msg = NULL;
    // Same low-address guard as __cajeta_is_unrecoverable: legacy int
    // throws produce non-pointer "throwables" that we must not
    // dereference. Skip the message read in that case; print the bare
    // value as a hex fallback. Reads on legitimate Throwable instances
    // (heap-allocated, vtable + message at slots 0/1) work as expected.
    if (value && (uintptr_t) value >= 4096) {
        msg = ((void**) value)[1];
    }
    // write(2), not fprintf(stderr): the caller abort()s (unrecoverable) or
    // exit()s, and abort() doesn't flush stdio. On Windows stderr is block-
    // buffered when piped (e.g. under a gtest death test), so an fprintf'd
    // message never reaches the fd. Format into a stack buffer, then write the
    // raw bytes to fd 2 directly.
    char buf[1024];
    int n;
    if (msg) {
        n = snprintf(buf, sizeof(buf), "cajeta: %s exception: %s\n",
                     kind, (const char*) msg);
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
    fi->isSymlink = S_ISLNK(st.st_mode) ? 1 : 0;
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
// cajeta.xpu.core runtime stubs (CajetaXPU phases 1-2, step 2).
//
// The cajeta.xpu.core stdlib classes (Stream / Event / Fence / Thread /
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
    int (*cuModuleLoadData)(void**, const void*);
    int (*cuModuleGetFunction)(void**, void*, const char*);
    int (*cuMemAlloc)(cajeta_cudeviceptr*, size_t);
    int (*cuMemcpyHtoD)(cajeta_cudeviceptr, const void*, size_t);
    int (*cuMemcpyDtoH)(void*, cajeta_cudeviceptr, size_t);
    int (*cuMemFree)(cajeta_cudeviceptr);
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
    CAJ_BIND(cuModuleLoadData, "cuModuleLoadData");
    CAJ_BIND(cuModuleGetFunction, "cuModuleGetFunction");
    CAJ_BIND(cuMemAlloc, "cuMemAlloc_v2");
    CAJ_BIND(cuMemcpyHtoD, "cuMemcpyHtoD_v2");
    CAJ_BIND(cuMemcpyDtoH, "cuMemcpyDtoH_v2");
    CAJ_BIND(cuMemFree, "cuMemFree_v2");
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
};
static struct cajeta_hip_api g_xpu_hip;

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
struct cajeta_kparams {
    char name[256];
    int count;
    const uint8_t* isBuffer;
    const uint32_t* byteSize;
};
#define CAJETA_XPU_MAX_KPARAMS 128
static struct cajeta_kparams g_xpu_kparams[CAJETA_XPU_MAX_KPARAMS];
static int g_xpu_kparam_count;

void __cajeta_xpu_register_kernel_params(const char* name, int32_t count,
                                         const uint8_t* isBuffer,
                                         const uint32_t* byteSize) {
    if (!name) return;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    if (g_xpu_kparam_count < CAJETA_XPU_MAX_KPARAMS) {
        struct cajeta_kparams* e = &g_xpu_kparams[g_xpu_kparam_count++];
        strncpy(e->name, name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        e->count = count;
        e->isBuffer = isBuffer;
        e->byteSize = byteSize;
    }
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
};
static struct cajeta_vk g_xpu_vk;

struct cajeta_vk_buf {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapped;
    VkDeviceSize size;
    int live;
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

    const char* libnames[2] = {"libvulkan.so.1", "libvulkan.so"};
    for (int i = 0; i < 2 && !g_xpu_vk.lib; ++i)
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
    g_xpu_vk.vkBindBufferMemory(g_xpu_vk.device, buf, mem, 0);
    void* mapped = NULL;
    if (g_xpu_vk.vkMapMemory(g_xpu_vk.device, mem, 0, VK_WHOLE_SIZE, 0, &mapped)
            != VK_SUCCESS) {
        g_xpu_vk.vkFreeMemory(g_xpu_vk.device, mem, NULL);
        g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, buf, NULL);
        return 0;
    }
    int slot = -1;
    for (int i = 0; i < g_vk_buf_count; ++i)
        if (!g_vk_bufs[i].live) { slot = i; break; }
    if (slot < 0) {
        if (g_vk_buf_count >= CAJETA_VK_MAX_BUFFERS) {
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
    return (int64_t) (slot + 1);
}

static struct cajeta_vk_buf* cajeta_xpu_vk_rec(int64_t handle) {
    if (handle <= 0 || handle > g_vk_buf_count) return NULL;
    struct cajeta_vk_buf* r = &g_vk_bufs[handle - 1];
    return r->live ? r : NULL;
}
static void* cajeta_xpu_vk_mapped(int64_t handle) {
    struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(handle);
    return r ? r->mapped : NULL;
}
static void cajeta_xpu_vk_free(int64_t handle) {
    struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(handle);
    if (!r) return;
    if (r->mapped) g_xpu_vk.vkUnmapMemory(g_xpu_vk.device, r->memory);
    if (r->buffer) g_xpu_vk.vkDestroyBuffer(g_xpu_vk.device, r->buffer, NULL);
    if (r->memory) g_xpu_vk.vkFreeMemory(g_xpu_vk.device, r->memory, NULL);
    r->live = 0; r->mapped = NULL; r->buffer = VK_NULL_HANDLE;
    r->memory = VK_NULL_HANDLE;
}

// One dispatch: shader module + descriptor set (binding i = bindings[i]) +
// pipeline + command buffer + submit + wait. `bindings` are 1-based table
// handles, in kernel-parameter order. Mirrors VulkanDriver::launch.
static int cajeta_xpu_vk_launch(const void* spirv, uint64_t len,
                                const char* entry, const int64_t* bindings,
                                int n, unsigned groups) {
    if (!spirv || len < 4 || n <= 0) return 0;
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
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
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

    VkComputePipelineCreateInfo cpci;
    memset(&cpci, 0, sizeof(cpci));
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module;
    cpci.stage.pName = entry;
    cpci.layout = pipeLayout;
    if (g_xpu_vk.vkCreateComputePipelines(g_xpu_vk.device, VK_NULL_HANDLE, 1,
                                          &cpci, NULL, &pipeline) != VK_SUCCESS)
        goto done;

    VkDescriptorPoolSize poolSize;
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = (uint32_t) n;
    VkDescriptorPoolCreateInfo dpci;
    memset(&dpci, 0, sizeof(dpci));
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;
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
    VkWriteDescriptorSet writes[64];
    memset(writes, 0, sizeof(writes[0]) * n);
    for (int i = 0; i < n; ++i) {
        struct cajeta_vk_buf* r = cajeta_xpu_vk_rec(bindings[i]);
        if (!r) goto done;
        bufInfos[i].buffer = r->buffer;
        bufInfos[i].offset = 0;
        bufInfos[i].range = VK_WHOLE_SIZE;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descSet;
        writes[i].dstBinding = (uint32_t) i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufInfos[i];
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
    g_xpu_vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    g_xpu_vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pipeLayout, 0, 1, &descSet, 0, NULL);
    g_xpu_vk.vkCmdDispatch(cmd, groups, 1, 1);
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
    return ok;
}

#else  // no Vulkan SDK header at runtime-build time — Vulkan unavailable.
static int cajeta_xpu_vulkan_init_locked(void) { return 0; }
static int64_t cajeta_xpu_vk_alloc(uint64_t b) { (void) b; return 0; }
static void* cajeta_xpu_vk_mapped(int64_t h) { (void) h; return NULL; }
static void cajeta_xpu_vk_free(int64_t h) { (void) h; }
static int cajeta_xpu_vk_launch(const void* s, uint64_t l, const char* e,
                                const int64_t* b, int n, unsigned g) {
    (void) s; (void) l; (void) e; (void) b; (void) n; (void) g; return 0;
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
// v1 uses the default stream; current()/create() return a null handle (the
// CUDA default stream) and the launch path passes NULL. sync() drains the
// context, which also releases the deferred launch borrows on the host side.
void* __cajeta_xpu_stream_current(void) { return NULL; }
void* __cajeta_xpu_stream_create(void) { return NULL; }
// Defined with the backend dispatcher below (after the kernel registries).
static void cajeta_xpu_sync_active(void);

void __cajeta_xpu_stream_sync(void* self) {
    (void) self;
    cajeta_xpu_sync_active();
}
void __cajeta_xpu_stream_wait_for(void* self, void* event) {
    (void)self; (void)event;
}
void __cajeta_xpu_stream_destroy(void* self) { (void)self; }

// --- Event -----------------------------------------------------------------
void* __cajeta_xpu_event_create(void) { return NULL; }
void __cajeta_xpu_event_record(void* self, void* stream) {
    (void)self; (void)stream;
}
void __cajeta_xpu_event_wait(void* self) { (void)self; }
bool __cajeta_xpu_event_query(void* self) { (void)self; return false; }
void __cajeta_xpu_event_destroy(void* self) { (void)self; }

// --- Fence -----------------------------------------------------------------
void* __cajeta_xpu_fence_create(void) { return NULL; }
void __cajeta_xpu_fence_signal(void* self, void* stream) {
    (void)self; (void)stream;
}
void __cajeta_xpu_fence_wait(void* self) { (void)self; }
bool __cajeta_xpu_fence_query(void* self) { (void)self; return false; }
void __cajeta_xpu_fence_destroy(void* self) { (void)self; }

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
uint32_t __cajeta_xpu_wave_shuffle_sync_u32(uint32_t value, uint32_t srcLane) {
    (void)srcLane; return value;
}
uint64_t __cajeta_xpu_wave_ballot_sync(bool predicate) {
    return predicate ? 1ULL : 0ULL;
}
// Single-lane wave (width=1) on CPU emulation: the wave-wide sum of one lane's
// value is just that value.
uint32_t __cajeta_xpu_wave_reduce_sum_u32(uint32_t value) { return value; }

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
        g_cpu_kernels[g_cpu_kernel_count].name = name;
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

// One worker's slice of the grid: blocks [cxStart, cxEnd), each blockX threads.
struct cajeta_cpu_grid_slice {
    cajeta_cpu_launch_fn fn;
    void** argv;
    int32_t blockX;
    int32_t cxStart;
    int32_t cxEnd;
};

// Run a contiguous slice of blocks. The launcher thunk is the per-BLOCK wrapper
// (Inc 5B): it loops the block's work-items internally (vectorized), so we call
// it ONCE PER BLOCK, setting ctaid.x + ntid.x. coord = [tid.xyz (unused here),
// ctaid.xyz, ntid.xyz]. Each worker owns its coord[9] (no sharing); a
// data-parallel, barrier-free CPU kernel writes disjoint elements, so the
// fan-out is race-free for any kernel that is correct on a GPU.
static void cajeta_xpu_cpu_run_slice(const struct cajeta_cpu_grid_slice* s) {
    int32_t coord[9] = {0, 0, 0, 0, 0, 0, s->blockX, 1, 1};   // ntid=(blockX,1,1)
    for (int32_t cx = s->cxStart; cx < s->cxEnd; ++cx) {
        coord[3] = cx;                                        // ctaid.x
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
// than a small launch saves. A workgroup barrier would break this (work-items
// must rendezvous), but barriers raise XPU-N01 on the CPU backend, so a launched
// CPU kernel is always barrier-free and embarrassingly parallel. True
// wave=SIMD-lane vectorization (Inc 5B) layers on top of each work-item call.
#ifndef CAJETA_XPU_CPU_PARALLEL_THRESHOLD
#define CAJETA_XPU_CPU_PARALLEL_THRESHOLD 4096   /* work-items */
#endif
#define CAJETA_XPU_CPU_MAX_WORKERS 256

static void cajeta_xpu_launch_cpu(const char* name, int32_t gridX, int32_t blockX,
                                  void* argv) {
    void* p = __cajeta_xpu_lookup_cpu_kernel(name);
    if (!p) {
        fprintf(stderr, "cajeta.xpu: no registered CPU kernel '%s' to launch\n",
                name);
        return;
    }
    cajeta_cpu_launch_fn fn = (cajeta_cpu_launch_fn) p;

    // CAJETA_XPU_CPU_SERIAL forces single-threaded execution — a deterministic
    // debug/oracle mode and the serial baseline for benchmarking. Read once.
    static int force_serial = -1;
    if (force_serial < 0) force_serial = getenv("CAJETA_XPU_CPU_SERIAL") ? 1 : 0;

    int64_t total = (int64_t) (gridX > 0 ? gridX : 0) *
                    (int64_t) (blockX > 0 ? blockX : 0);
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1) cores = 1;
    int32_t nworkers = (int32_t) ((long) gridX < cores ? (long) gridX : cores);
    if (nworkers > CAJETA_XPU_CPU_MAX_WORKERS) nworkers = CAJETA_XPU_CPU_MAX_WORKERS;

    // Serial path: forced, tiny launch, single core, or a single block.
    if (force_serial || gridX <= 1 || blockX <= 0 || nworkers <= 1 ||
        total < CAJETA_XPU_CPU_PARALLEL_THRESHOLD) {
        struct cajeta_cpu_grid_slice all = {fn, (void**) argv, blockX, 0, gridX};
        cajeta_xpu_cpu_run_slice(&all);
        return;
    }

    // Parallel fan-out: chunk the gridX blocks evenly across `nworkers`.
    pthread_t threads[CAJETA_XPU_CPU_MAX_WORKERS];
    struct cajeta_cpu_grid_slice slices[CAJETA_XPU_CPU_MAX_WORKERS];
    char spawned[CAJETA_XPU_CPU_MAX_WORKERS];
    int32_t base = gridX / nworkers, rem = gridX % nworkers, cx = 0;
    for (int32_t i = 0; i < nworkers; ++i) {
        int32_t count = base + (i < rem ? 1 : 0);
        slices[i].fn = fn;
        slices[i].argv = (void**) argv;
        slices[i].blockX = blockX;
        slices[i].cxStart = cx;
        slices[i].cxEnd = cx + count;
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
int64_t __cajeta_xpu_buffer_alloc(void* self, uint64_t byteCount) {
    (void) self;
    if (byteCount == 0) return 0;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA: {
            cajeta_cudeviceptr p = 0;
            if (g_xpu_cuda.cuMemAlloc(&p, (size_t) byteCount) != 0) return 0;
            return (int64_t) p;
        }
        case CAJ_XPU_HIP: {
            void* p = NULL;
            if (g_xpu_hip.hipMalloc(&p, (size_t) byteCount) != 0) return 0;
            return (int64_t) (intptr_t) p;
        }
        case CAJ_XPU_VULKAN:
            return cajeta_xpu_vk_alloc(byteCount);   // handle = buffer-table index
        case CAJ_XPU_CPU: {
            void* p = malloc((size_t) byteCount);   // CPU "device" memory = host
            return (int64_t) (intptr_t) p;
        }
        default: return 0;   // none: diagnostic emitted
    }
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
void __cajeta_xpu_buffer_free(void* self, int64_t handle) {
    (void) self;
    if (!handle) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:   g_xpu_cuda.cuMemFree((cajeta_cudeviceptr) handle); return;
        case CAJ_XPU_HIP:    g_xpu_hip.hipFree((void*) (intptr_t) handle); return;
        case CAJ_XPU_VULKAN: cajeta_xpu_vk_free(handle); return;
        case CAJ_XPU_CPU:    free((void*) (intptr_t) handle); return;
        default: return;
    }
}

// --- Launch + module registration -------------------------------------------
// The compiler lowers `kernel.launch(stream, grid:, block:)(args)` to a call
// here, passing the kernel's PTX entry name, 1-D grid/block, and the CUDA
// kernelParams argv (an array of pointers to each argument value). The real
// NVPTX path (cuLaunchKernel via the dlopen'd driver) lands in the host-launch
// runtime; this is the not-yet-wired no-op so the symbol resolves and host
// codegen of a launch site links.
// CUDA launch: lazily load the module + resolve the function, then 1-D launch.
static void cajeta_xpu_launch_cuda(const char* kernelName, int32_t gridX,
                                   int32_t blockX, uint32_t sharedBytes,
                                   void* argv) {
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
    // 1-D grid/block; default stream; kernelParams = the CUDA argv the launch
    // site marshalled (pointers to each arg value). sharedBytes sizes the
    // kernel's dynamic (extern) shared memory; 0 for static-only kernels.
    g_xpu_cuda.cuLaunchKernel(fn, (unsigned) gridX, 1, 1,
                              (unsigned) blockX, 1, 1,
                              (unsigned) sharedBytes, /*stream=*/NULL,
                              (void**) argv, /*extra=*/NULL);
}

// HIP launch: lazily load the hsaco module + resolve the function (reusing the
// shared module table — only one device backend is active per run), then 1-D
// launch. Mirrors cajeta_xpu_launch_cuda with hip* entry points.
static void cajeta_xpu_launch_hip(const char* kernelName, int32_t gridX,
                                  int32_t blockX, uint32_t sharedBytes,
                                  void* argv) {
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
    g_xpu_hip.hipModuleLaunchKernel(fn, (unsigned) gridX, 1, 1,
                                    (unsigned) blockX, 1, 1,
                                    (unsigned) sharedBytes, /*stream=*/NULL,
                                    (void**) argv, /*extra=*/NULL);
}

// Vulkan launch: translate the uniform kernelParams argv into descriptor
// bindings using the per-kernel param metadata — buffer args map to their
// existing storage buffers (argv slot holds the buffer-table handle), scalar
// args are copied into transient single-element SSBOs (freed after) — then
// dispatch gridX work-groups (the local size is baked into the SPIR-V). This is
// the one backend whose launch ABI forks from the pointer-arg kernelParams
// model: Vulkan's compute entry has no params, only descriptor bindings.
static void cajeta_xpu_launch_vulkan(const char* kernelName, int32_t gridX,
                                     void* argvv) {
    void** argv = (void**) argvv;
    pthread_mutex_lock(&g_xpu_cuda_lock);
    struct cajeta_xpu_module* e = cajeta_xpu_find_module(kernelName);
    const void* spirv = e ? e->image : NULL;
    uint64_t len = e ? e->len : 0;
    pthread_mutex_unlock(&g_xpu_cuda_lock);
    if (!spirv || len < 4) {
        fprintf(stderr,
                "cajeta.xpu: no registered SPIR-V kernel '%s' to launch\n",
                kernelName);
        return;
    }
    struct cajeta_kparams* kp = cajeta_xpu_find_kparams(kernelName);
    if (!kp || kp->count <= 0 || kp->count > 64) {
        fprintf(stderr,
                "cajeta.xpu: missing/invalid parameter metadata for Vulkan "
                "kernel '%s'\n", kernelName);
        return;
    }
    const int n = kp->count;
    int64_t bindings[64];
    int64_t transient[64];
    int ntrans = 0;
    int built = 1;
    for (int i = 0; i < n; ++i) {
        if (kp->isBuffer[i]) {
            bindings[i] = *(int64_t*) argv[i];    // existing storage buffer
        } else {
            uint32_t sz = kp->byteSize[i] ? kp->byteSize[i] : 4u;
            int64_t h = cajeta_xpu_vk_alloc(sz);  // transient scalar SSBO
            if (!h) { built = 0; break; }
            void* m = cajeta_xpu_vk_mapped(h);
            if (m) memcpy(m, argv[i], sz);
            bindings[i] = h;
            transient[ntrans++] = h;
        }
    }
    if (built)
        cajeta_xpu_vk_launch(spirv, len, kernelName, bindings, n,
                             (unsigned) gridX);
    for (int i = 0; i < ntrans; ++i) cajeta_xpu_vk_free(transient[i]);
}

// The host-source `kernel.launch(...)` entry point: dispatch to the active
// backend (chosen + cached on first device touch).
void __cajeta_xpu_launch(const char* kernelName, int32_t gridX, int32_t blockX,
                         uint32_t sharedBytes, void* argv) {
    if (!kernelName) return;
    switch (cajeta_xpu_active_backend()) {
        case CAJ_XPU_CUDA:
            cajeta_xpu_launch_cuda(kernelName, gridX, blockX, sharedBytes, argv);
            return;
        case CAJ_XPU_HIP:
            cajeta_xpu_launch_hip(kernelName, gridX, blockX, sharedBytes, argv);
            return;
        case CAJ_XPU_VULKAN:
            cajeta_xpu_launch_vulkan(kernelName, gridX, argv);
            return;
        case CAJ_XPU_CPU:
            cajeta_xpu_launch_cpu(kernelName, gridX, blockX, argv);
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
