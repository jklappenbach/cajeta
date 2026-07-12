// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
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

// SIGSEGV / SIGBUS backtrace handler (POSIX). SIGABRT above catches glibc
// heap-corruption aborts; a SIGSEGV/SIGBUS is the OTHER way a memory bug
// surfaces — a wild pointer, a use-after-free deref, or a fiber-stack
// overflow hitting the guard page (__cajeta_fiber_stack_alloc). Without a
// handler those die silently with just "exit 139", which is exactly what the
// parallel-stream crashes on aarch64 do — no location, no context. This
// prints the faulting address, the running carrier/fiber, and a native
// backtrace to stderr (captured per-test by KEEP_LOGS in CI), then re-raises
// the default action so the process still dies with the right signal.
//
// Signal-safety: backtrace()/backtrace_symbols_fd() are async-signal-safe
// (no malloc, write directly to the fd); the fprintf lines match the SIGABRT
// handler's existing (accepted) practice. The handler runs on an alternate
// stack (sigaltstack + SA_ONSTACK) so a stack-overflow fault — where the
// normal stack is unusable — can still report.
#if !defined(_WIN32)

static struct sigaction __cajeta_prev_sigsegv;
static struct sigaction __cajeta_prev_sigbus;

static void __cajeta_segv_handler(int signo, siginfo_t* info, void* uctx) {
    (void) uctx;
    const char* name = (signo == SIGBUS) ? "SIGBUS" : "SIGSEGV";
    fprintf(stderr, "\ncajeta: %s caught — fault addr %p\n",
            name, info ? info->si_addr : NULL);
    // Running context, if any. __cajeta_current_fiber / __cajeta_my_carrier
    // are this TU's TLS; reading them here is best-effort diagnostic.
    struct cajeta_carrier* c = __cajeta_my_carrier;
    struct cajeta_fiber* f = __cajeta_current_fiber;
    fprintf(stderr, "cajeta: carrier=%d fiber=%d\n",
            c ? c->carrier_id : -1, f ? f->dbg_id : 0);
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2 /*stderr*/);
    // Dump the running thread's drop chain — its entries carry cajeta source
    // tags (file:line of the owner being dropped). If the fault is mid-drop
    // (a double-drop / freed-then-freed-again pointer, which the libc free
    // frames in the aarch64 backtraces suggest), this names the cajeta-level
    // objects in flight — the one piece of source-level context a JIT'd
    // backtrace can't give.
    __cajeta_dump_drop_chain();
    // Chain to the previous handler, else restore default and re-raise so the
    // process dies with the correct signal (and CI records exit 139/138).
    struct sigaction* prev =
        (signo == SIGBUS) ? &__cajeta_prev_sigbus : &__cajeta_prev_sigsegv;
    if (prev->sa_flags & SA_SIGINFO) {
        if (prev->sa_sigaction) { prev->sa_sigaction(signo, info, uctx); return; }
    } else if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN
               && prev->sa_handler != NULL) {
        prev->sa_handler(signo); return;
    }
    signal(signo, SIG_DFL);
    raise(signo);
}

void __cajeta_install_segv_handler(void) {
    // Same "already installed?" guard as the SIGABRT path: each JIT module's
    // runtime copy runs its own constructor, and chaining a handler whose code
    // later unmaps would crash. The host's lifetime-stable static copy wins.
    struct sigaction cur;
    if (sigaction(SIGSEGV, NULL, &cur) == 0) {
        bool already = (cur.sa_flags & SA_SIGINFO)
            ? (cur.sa_sigaction != NULL)
            : (cur.sa_handler != SIG_DFL && cur.sa_handler != SIG_IGN
               && cur.sa_handler != NULL);
        if (already) return;
    }
    // Alternate signal stack so a stack-overflow fault can still be reported.
    // Fixed 64 KiB — SIGSTKSZ is no longer a compile-time constant on modern
    // glibc, and 64 KiB comfortably covers backtrace()'s frame needs.
    static char altstack[65536];
    stack_t ss;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    sa.sa_sigaction = __cajeta_segv_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &__cajeta_prev_sigsegv);
    sigaction(SIGBUS, &sa, &__cajeta_prev_sigbus);
}

#else
void __cajeta_install_segv_handler(void) {}
#endif

// Auto-install at runtime load. Constructor runs before main(); the handler
// is then armed for the program lifetime, including before any Cajeta code
// has executed (so a stdlib-load-time abort is also caught).
__attribute__((constructor))
static void __cajeta_runtime_init(void) {
    __cajeta_install_sigabrt_handler();
    __cajeta_install_segv_handler();
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
//
// '#' bytes are skipped (mode-erased dispatch, element-ownership spec
// §5.1.4): owning and borrowing instantiations of one template share slot
// layout and must dispatch interchangeably from mode-agnostic template
// bodies. Must stay in lockstep with the two compiler-side copies
// (CajetaClass.cpp, StructureMetadata.cpp).
int64_t __cajeta_signature_hash(const char* s) {
    if (!s) return 0;
    uint64_t h = 0xcbf29ce484222325ULL;     // FNV offset basis
    while (*s) {
        uint8_t c = (uint8_t) *s++;
        if (c == '#') continue;
        h ^= c;
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
//   [24..31] ptr classObject          (this class's cached cajeta.reflect.Class
//                                      instance — Object.getClass() loads it;
//                                      NULL for value types / pre-reflect classes)
//   [32..]   [count x { i64 hash, ptr fn }] entries
#define CAJETA_VTABLE_PARENT_OFFSET 8
#define CAJETA_VTABLE_DROP_FN_OFFSET 16
#define CAJETA_VTABLE_CLASSOBJECT_OFFSET 24
#define CAJETA_VTABLE_ENTRIES_OFFSET 32

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

// ---- Reflection (cajeta.reflect) — REFL-1 -------------------------------
//
// Fixed-layout C mirrors of the RTTI structs emitted by
// StructureMetadata.cpp (getRttiStructType / getFieldStructType /
// getMethodStructType / getParameterStructType). These MUST stay in
// lock-step with those LLVM struct shapes — the compiler builds the data,
// these readers walk it. All variable-length data (names, tables, annotation
// and parent lists) is referenced by pointer, so every field below sits at a
// fixed offset regardless of the class.

// REFL-6b annotation argument descriptors. The `annotations` pointer in every
// owner descriptor below points at a [N x CajetaAnnotationDesc]; each annotation
// in turn references its [argCount x CajetaAnnotationArgDesc]. MUST stay in
// lock-step with getAnnotationStructType / getAnnotationArgStructType in
// StructureMetadata.cpp.
//
// `kind` mirrors AnnotationArgKind (Annotatable.h). String/ClassRef payloads
// live in strVal; Int64 in i64Val; Bool in boolVal. List kinds are recorded by
// kind (argCount stays accurate) but carry no element data — only the scalar
// accessors are surfaced this increment.
enum {
    CAJETA_AK_INT64      = 0,
    CAJETA_AK_STRING     = 1,
    CAJETA_AK_BOOL       = 2,
    CAJETA_AK_CLASSREF   = 3,
    CAJETA_AK_INT64LIST  = 4,
    CAJETA_AK_STRINGLIST = 5,
    CAJETA_AK_BOOLLIST   = 6,
};

typedef struct {
    const char* name;        // argument key ("" for the unnamed single-arg form)
    int32_t     kind;        // CAJETA_AK_*
    int64_t     i64Val;
    const char* strVal;      // String payload; type name for ClassRef; NULL otherwise
    int8_t      boolVal;
    int32_t     listCount;   // element count for the *List kinds; 0 otherwise
    const void* listData;    // [N x int64] / [N x char*] / [N x int8] by kind
} CajetaAnnotationArgDesc;

typedef struct {
    const char*                    name;      // annotation canonical type name
    int16_t                        argCount;
    const CajetaAnnotationArgDesc* args;       // NULL when argCount == 0
} CajetaAnnotationDesc;

// Parameter descriptor — the ORIGINAL 5-field shape (#ParameterDesc). Kept
// separate from CajetaFieldDesc, which gained byteOffset/typeFlags for fields.
typedef struct {
    const char*  name;
    const char*  type;
    int32_t      modifiers;
    int16_t      annotationCount;
    const CajetaAnnotationDesc* annotations;
} CajetaParamDesc;

// Field descriptor (#FieldDesc). MUST match getFieldStructType() in
// StructureMetadata.cpp: { ptr, ptr, i32, i16, ptr, i32, i64 }.
typedef struct {
    const char*  name;
    const char*  type;
    int32_t      modifiers;
    int16_t      annotationCount;
    const CajetaAnnotationDesc* annotations;
    int32_t      byteOffset;        // offset in the instance struct; -1 if static
    int64_t      typeFlags;         // field type's CajetaType TYPE_ID flag word
} CajetaFieldDesc;

typedef struct {
    const char*            name;           // canonical signature
    const char*            returnType;
    int64_t                sigHash;        // FNV-1a of toCanonical(false)
    int32_t                modifiers;
    int16_t                parameterCount;
    const CajetaParamDesc* parameters;
    int16_t                annotationCount; // REFL-6a: method/ctor annotation names
    const CajetaAnnotationDesc* annotations; // REFL-6b: names + arg values (NULL if none)
} CajetaMethodDesc;

// REFL-7: one declared template parameter (`<T>`, `<T extends Foo & Bar>`, or a
// non-type `<uint32 N>`). MUST match getTemplateParamStructType() in
// StructureMetadata.cpp.
typedef struct {
    const char*  name;              // parameter name, e.g. "T"
    int16_t      boundCount;
    const char** bounds;            // canonical bound type names (NULL if none)
    int8_t       isNonType;         // 1 for a `<uint32 N>` value parameter
    const char*  nonTypePrimitive;  // declared primitive when isNonType, else NULL
} CajetaTemplateParamDesc;

typedef struct {
    int64_t                 allocationSize;
    const char*             typeName;
    int32_t                 modifiers;
    int16_t                 classAnnotationCount;
    const CajetaAnnotationDesc* classAnnotations;
    int16_t                 propertyCount;
    const CajetaFieldDesc*  properties;
    int16_t                 methodCount;
    const CajetaMethodDesc* methods;
    int16_t                 parentCount;
    const char**            parentNames;
    void*                   vtable;
    void*                   invokeAdapter;       // REFL-2 reflective invoke adapter (or NULL)
    void*                   newInstanceAdapter;  // REFL-2C reflective ctor adapter (or NULL)
    int16_t                 constructorCount;
    const CajetaMethodDesc* constructors;        // #MethodDesc[] for constructors
    // REFL-7 template reflection. templateParams are the `<T>` declarations (on
    // both the template and its instantiations); templateArgs are the concrete
    // type names an instantiation was materialized with (e.g. "cajeta.int32"
    // for Box<int32>) — empty for a non-template class or an unmaterialized
    // template.
    int16_t                       templateParamCount;
    const CajetaTemplateParamDesc* templateParams;
    int16_t                       templateArgCount;
    const char**                  templateArgs;
} CajetaRtti;

// Object.getClass(): obj -> its cached #ClassObject (the cajeta.reflect.Class
// instance) through the vtable's classObject slot. The returned pointer is a
// borrow of a process-lifetime static; the caller never frees it.
void* __cajeta_object_get_class(void* obj) {
    if (!obj) return NULL;
    void* vtable = *(void**) obj;                 // header slot 0
    if (!vtable) return NULL;
    return *(void**) ((char*) vtable + CAJETA_VTABLE_CLASSOBJECT_OFFSET);
}

// Reified template capture (reified-capture-spec.md): does object `obj`'s runtime
// instantiation match — or descend one declared level from — the canonical type
// name `targetName`? Because cajeta monomorphizes, each instantiation
// (`Tensor<float32>` vs `Tensor<int32>`) carries a distinct RTTI whose typeName
// IS its full canonical name, so an exact instantiation test is a string compare;
// a one-level is-a also matches a direct parent name. Walks
// obj -> vtable(slot 0) -> classObject(+CAJETA_VTABLE_CLASSOBJECT_OFFSET) ->
// rtti(classObject+8). Returns 1 on match, 0 otherwise. Null-safe — the backbone
// of `instanceof Tensor<float32>` and the `(Tensor<float32>) w` capture cast.
int32_t __cajeta_instanceof_named(void* obj, const char* targetName) {
    if (!obj || !targetName) return 0;
    void* vtable = *(void**) obj;                 // header slot 0
    if ((uintptr_t) vtable < 4096) return 0;      // not a real vtable pointer
    void* classObject =
        *(void**) ((char*) vtable + CAJETA_VTABLE_CLASSOBJECT_OFFSET);
    if (!classObject) return 0;
    void* rtti = *(void**) ((char*) classObject + 8);
    if (!rtti) return 0;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (r->typeName && strcmp(r->typeName, targetName) == 0) return 1;
    for (int32_t i = 0; i < r->parentCount; ++i) {
        if (r->parentNames && r->parentNames[i]
                && strcmp(r->parentNames[i], targetName) == 0) {
            return 1;
        }
    }
    return 0;
}

// --- cajeta.reflect class registry (REFL-8) --------------------------------
// Maps a class's canonical name -> its cached #ClassObject (the reflect Class
// instance), so Class.forName(name) can resolve a class from a string with no
// live instance in hand. Populated at startup: the compiler emits, per class,
// an llvm.global_ctors entry calling __cajeta_register_class(name, classObject)
// (StructureMetadata::populate). A growable, process-lifetime table — never
// freed; last-writer-wins on a duplicate canonical name (harmless — the same
// class object is registered once per definition site). Nothing strips classes
// today, so every compiled class is registered; @Retained is the advisory marker
// for the future AOT linker (it must keep marked classes even when unreferenced).
static struct { const char* name; void* classObject; }* g_cajeta_classes = NULL;
static int g_cajeta_class_count = 0;
static int g_cajeta_class_cap   = 0;

void __cajeta_register_class(const char* name, void* classObject) {
    if (!name || !classObject) return;
    for (int i = 0; i < g_cajeta_class_count; ++i) {
        if (g_cajeta_classes[i].name && strcmp(g_cajeta_classes[i].name, name) == 0) {
            g_cajeta_classes[i].classObject = classObject;  // last writer wins
            return;
        }
    }
    if (g_cajeta_class_count == g_cajeta_class_cap) {
        int newCap = g_cajeta_class_cap ? g_cajeta_class_cap * 2 : 64;
        void* grown = realloc(g_cajeta_classes,
                              (size_t) newCap * sizeof(*g_cajeta_classes));
        if (!grown) return;            // OOM: drop the registration, don't crash
        g_cajeta_classes = grown;
        g_cajeta_class_cap = newCap;
    }
    // strdup the key: a JIT'd registration ctor's name global lives in JIT
    // memory that may be torn down, leaving a dangling key (the same hazard the
    // CPU-kernel registry documents above). Process-lifetime copy.
    g_cajeta_classes[g_cajeta_class_count].name = strdup(name);
    g_cajeta_classes[g_cajeta_class_count].classObject = classObject;
    ++g_cajeta_class_count;
}

// Class.forName backend. `nameBytes` is a cajeta int8[] ({ i64 count,
// [count x i8] }) — the canonical name's UTF-8 bytes. Single-parameter so it can
// return a Class borrow (a multi-param @Native returning a borrow is rejected,
// CAJETA_ERROR_BORROW_RETURN_MULTI_PARAM); the length comes from the array's
// count header. Returns the #ClassObject pointer (a borrow of a process-lifetime
// static) or NULL when no class with that canonical name is registered.
void* __cajeta_class_for_name(void* nameBytes) {
    if (!nameBytes) return NULL;
    int64_t len = *((int64_t*) nameBytes);              // the i64 count header
    if (len < 0) return NULL;
    const char* data = (const char*) nameBytes + 8;
    for (int i = 0; i < g_cajeta_class_count; ++i) {
        const char* n = g_cajeta_classes[i].name;
        if (n && (int64_t) strlen(n) == len && memcmp(n, data, (size_t) len) == 0)
            return g_cajeta_classes[i].classObject;
    }
    return NULL;
}

// name -> RTTI lookup (reified-capture: class-bounded-wildcard precursor).
// Resolves a type's canonical name (a plain C string — the form a capture-site
// lowering holds for a container's element type) to that type's CajetaRtti*,
// so `(Foo<? extends Animal>) w` can recover the element type's hierarchy at
// runtime and bound-check it (the container RTTI stores only the element's NAME
// string, not its RTTI pointer). Reuses the REFL-8 name->#ClassObject table:
// #ClassObject = { ptr Class#VTable, ptr rtti }, so the rtti is at offset 8 (the
// same hop __cajeta_instanceof_named makes). Returns a borrow of a
// process-lifetime static, or NULL when no class with that canonical name is
// registered (e.g. DCE'd under Lean, or never compiled) — callers must treat a
// NULL as "cannot prove the bound" and fail the capture safely. Also strengthens
// reflection (a name->rtti primitive for the reflective layer).
#define CAJETA_CLASSOBJECT_RTTI_OFFSET 8
// `used, retain` (external-debug §4): the debugger's parent-chain walk goes
// parent_name -> here, and a program that never reflects calls this from nowhere,
// so DCE / --gc-sections would drop it from an AOT binary.
__attribute__((used, retain))
void* __cajeta_rtti_for_name(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < g_cajeta_class_count; ++i) {
        const char* n = g_cajeta_classes[i].name;
        if (n && strcmp(n, name) == 0) {
            void* classObject = g_cajeta_classes[i].classObject;
            if (!classObject) return NULL;
            return *(void**) ((char*) classObject + CAJETA_CLASSOBJECT_RTTI_OFFSET);
        }
    }
    return NULL;
}

// REFL-10: registry enumeration for Class.allClasses / classesInPackage /
// classesAnnotated. The filtering (package match, annotation match) is done in
// cajeta over getName()/hasAnnotation(); these two primitives just expose the
// registry as an indexable list. __cajeta_class_at returns a #ClassObject
// pointer (a borrow of a process-lifetime static) or NULL when out of range.
int32_t __cajeta_class_count(void) {
    return (int32_t) g_cajeta_class_count;
}
void* __cajeta_class_at(int32_t idx) {
    if (idx < 0 || idx >= g_cajeta_class_count) return NULL;
    return g_cajeta_classes[idx].classObject;
}

// REFL-12 bounded reflection: is `leafRtti`'s type the same as, or a descendant
// of, `boundRtti`'s type? `leafRtti`/`boundRtti` are CajetaRtti* (the #RttiGlobal
// a Class instance holds in its `rtti` field). The bound is the compile-time `T`
// in `Class<T>`; the leaf is resolved at runtime (e.g. forName of a string). The
// check walks the leaf TYPE's actual vtable parent chain (each rtti carries its
// type's `vtable`; the chain link is CAJETA_VTABLE_PARENT_OFFSET) for the bound
// type's vtable — the same proven walk __cajeta_exc_matches uses for try/catch.
// Returns 1 iff leaf <: bound, else 0. Defensive: depth-capped, address-guarded.
int32_t __cajeta_is_subtype(void* leafRtti, void* boundRtti) {
    if (!leafRtti || !boundRtti) return 0;
    void* boundVtable = ((CajetaRtti*) boundRtti)->vtable;
    if (!boundVtable) return 0;
    void* vtable = ((CajetaRtti*) leafRtti)->vtable;
    for (int depth = 0; depth < 256; ++depth) {
        if ((uintptr_t) vtable < 4096) break;
        if (vtable == boundVtable) return 1;
        vtable = *(void**) ((char*) vtable + CAJETA_VTABLE_PARENT_OFFSET);
    }
    return 0;
}

// external-debug §4.1.2: the same hop, exported. A debugger holds only a local's
// ADDRESS; this is how it reaches the object's DYNAMIC type (a `Shape s` holding
// a `Circle` must render as a Circle) and, through the RTTI, that type's field
// names, byte offsets, and type flags. `used, retain` — nothing in generated code
// calls it, so DCE and --gc-sections would drop it from an AOT binary.
__attribute__((used, retain))
void* __cajeta_rtti_of(void* obj);

// obj -> its CajetaRtti* (the obj->vtable->classObject->rtti hop), or NULL when
// obj isn't a real heap object carrying a vtable/classObject/rtti.
static void* cajeta_rtti_from_obj(void* obj) {
    if (!obj) return NULL;
    void* vtable = *(void**) obj;                          // header slot 0
    if ((uintptr_t) vtable < 4096) return NULL;            // not a real vtable
    void* classObject =
        *(void**) ((char*) vtable + CAJETA_VTABLE_CLASSOBJECT_OFFSET);
    if (!classObject) return NULL;
    return *(void**) ((char*) classObject + CAJETA_CLASSOBJECT_RTTI_OFFSET);
}

__attribute__((used, retain))
void* __cajeta_rtti_of(void* obj) {
    return cajeta_rtti_from_obj(obj);
}

// True iff canonical instantiation name `typeName` (e.g. "test.Box<test.Dog>")
// has erased base exactly `baseName` (e.g. "test.Box") — i.e. typeName is
// baseName followed by '<' (an instantiation) or end-of-string (the raw base).
static int cajeta_base_name_matches(const char* typeName, const char* baseName) {
    size_t bl = strlen(baseName);
    if (strncmp(typeName, baseName, bl) != 0) return 0;
    char after = typeName[bl];
    return after == '<' || after == '\0';
}

// The last '.'-separated component of a canonical name ("cajeta.lang.Floating"
// -> "Floating"; "Floating" -> "Floating").
static const char* cajeta_last_name(const char* s) {
    const char* dot = strrchr(s, '.');
    return dot ? dot + 1 : s;
}

// Numeric-marker code for a bound's canonical name: 1=Numeric, 2=Floating,
// 3=Integral, 4=Complex; -1 if `boundName` is not a cajeta.lang numeric marker
// (so the caller uses the nominal class-subtype path instead).
static int cajeta_numeric_marker_code(const char* boundName) {
    const char* n = cajeta_last_name(boundName);
    if (strcmp(n, "Numeric")  == 0) return 1;
    if (strcmp(n, "Floating") == 0) return 2;
    if (strcmp(n, "Integral") == 0) return 3;
    if (strcmp(n, "Complex")  == 0) return 4;
    return -1;
}

// Numeric kind of a reified element TYPE NAME (the primitive name a container's
// RTTI records, e.g. "float32"/"bfloat16"/"int32"/"uint8"/"boolean"):
// 0=bool, 1=integral (signed/unsigned int), 2=float, 3=complex; -1 if it is not
// a known numeric primitive (e.g. a class element — fall through to the nominal
// path). Mirrors the FLAG-lattice kinds CajetaClass::satisfiesNumericMarker uses,
// resolved here from the name because primitives carry no class RTTI.
static int cajeta_numeric_kind_of(const char* t) {
    if (!t) return -1;
    if (strcmp(t, "boolean") == 0)    return 0;
    if (strncmp(t, "float",  5) == 0) return 2;   // float16/32/64/128, float8*/6*/4*
    if (strncmp(t, "bfloat", 6) == 0) return 2;   // bfloat16
    if (strncmp(t, "complex", 7) == 0) return 3;  // complex64/128 (reserved)
    if (strncmp(t, "uint", 4) == 0)   return 1;   // uint8..uint128
    if (strncmp(t, "int", 3) == 0)    return 1;   // int8..int128
    return -1;
}

// Does an element of numeric kind `elemKind` satisfy numeric marker `markerCode`?
// bool satisfies no numeric marker; Numeric admits integral/float/complex (not
// bool); Floating admits float; Integral admits int/uint; Complex admits complex.
static int cajeta_numeric_conforms(int elemKind, int markerCode) {
    if (elemKind == 0) return 0;                                  // bool: none
    if (markerCode == 1) return elemKind == 1 || elemKind == 2 || elemKind == 3;
    if (markerCode == 2) return elemKind == 2;
    if (markerCode == 3) return elemKind == 1;
    if (markerCode == 4) return elemKind == 3;
    return 0;
}

// Class-bounded-wildcard reified match (reified-capture-spec.md §5): is `obj` an
// instance of `baseName<...>` whose reified template arg at `argIndex` conforms
// to `boundName` — element type == bound, satisfies a numeric marker bound, or
// element <: bound (nominal)? Backs `instanceof Foo<? extends Bound>` and the
// `(Foo<? extends Bound>) w` capture cast. The element type is recovered by NAME
// from the container's reified templateArgs; a **numeric-marker** bound
// (cajeta.lang.{Numeric,Floating,Integral,Complex}) is checked against the
// element's primitive KIND (reified-capture 5c / tensor 7c — primitives carry no
// class RTTI to walk), otherwise the name is resolved to its RTTI and walked as a
// nominal subtype. Null-safe; returns 0 ("doesn't match" / "cannot prove the
// bound") rather than ever admitting a mis-bounded value.
int32_t __cajeta_instanceof_bounded(void* obj, const char* baseName,
                                    int32_t argIndex, const char* boundName) {
    if (!obj || !baseName || !boundName) return 0;
    CajetaRtti* r = (CajetaRtti*) cajeta_rtti_from_obj(obj);
    if (!r || !r->typeName) return 0;
    if (!cajeta_base_name_matches(r->typeName, baseName)) return 0;
    if (argIndex < 0 || argIndex >= r->templateArgCount || !r->templateArgs)
        return 0;
    const char* elemName = r->templateArgs[argIndex];
    if (!elemName) return 0;
    if (strcmp(elemName, boundName) == 0) return 1;        // reflexive / exact
    // Numeric-marker bound on a primitive element: check the dtype kind by name.
    int markerCode = cajeta_numeric_marker_code(boundName);
    if (markerCode >= 0) {
        int elemKind = cajeta_numeric_kind_of(elemName);
        if (elemKind >= 0) return cajeta_numeric_conforms(elemKind, markerCode);
        // class element under a numeric marker: fall through to the nominal walk
        // (a class may nominally implement the marker).
    }
    void* elemRtti = __cajeta_rtti_for_name(elemName);
    void* boundRtti = __cajeta_rtti_for_name(boundName);
    if (!elemRtti || !boundRtti) return 0;                 // unresolved: fail safe
    return __cajeta_is_subtype(elemRtti, boundRtti);
}

// RTTI scalar readers — `rtti` is a CajetaRtti* (the #RttiGlobal address a
// Class instance holds in its `rtti` field).
int32_t __cajeta_rtti_field_count(void* rtti) {
    return rtti ? (int32_t) ((CajetaRtti*) rtti)->propertyCount : 0;
}
int32_t __cajeta_rtti_method_count(void* rtti) {
    return rtti ? (int32_t) ((CajetaRtti*) rtti)->methodCount : 0;
}
int32_t __cajeta_rtti_parent_count(void* rtti) {
    return rtti ? (int32_t) ((CajetaRtti*) rtti)->parentCount : 0;
}
int32_t __cajeta_rtti_modifiers(void* rtti) {
    return rtti ? ((CajetaRtti*) rtti)->modifiers : 0;
}
int64_t __cajeta_rtti_alloc_size(void* rtti) {
    return rtti ? ((CajetaRtti*) rtti)->allocationSize : 0;
}
int32_t __cajeta_rtti_name_len(void* rtti) {
    if (!rtti) return 0;
    const char* n = ((CajetaRtti*) rtti)->typeName;
    return n ? (int32_t) strlen(n) : 0;
}

// external-debug §4: the raw C string, for a debugger. The name_into form below
// writes into a cajeta int8[] — the right shape for cajeta callers, useless to
// gdb, which has no cajeta array to hand it. `used, retain`: nothing in generated
// code calls this.
__attribute__((used, retain))
const char* __cajeta_rtti_type_name(void* rtti) {
    if (!rtti) return "";
    const char* n = ((CajetaRtti*) rtti)->typeName;
    return n ? n : "";
}

// Field name, likewise raw. (field_name_into writes a cajeta int8[].)
__attribute__((used, retain))
const char* __cajeta_rtti_field_name(void* rtti, int32_t idx) {
    if (!rtti) return "";
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->propertyCount || !r->properties) return "";
    const char* n = r->properties[idx].name;
    return n ? n : "";
}

// A class's RTTI carries only its OWN fields — an inherited field lives on the
// parent's. Parents are recorded as NAMES, so the walk a debugger makes is
// parent_name -> __cajeta_rtti_for_name -> that RTTI's fields. Without this a
// `Circle` renders `radius` and silently loses the `sides` it inherits.
__attribute__((used, retain))
const char* __cajeta_rtti_parent_name(void* rtti, int32_t idx) {
    if (!rtti) return "";
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->parentCount || !r->parentNames) return "";
    const char* n = r->parentNames[idx];
    return n ? n : "";
}
// Copy the canonical type name into a caller-allocated int8[] (`out` =
// { i64 count, [count x i8] }), clamped to the array's capacity. Out-param
// rather than a returned int8[] because a `@Native` int8[] return isn't
// drop-tracked (see Sha256.cajeta); the caller does
// `int8[] out = heap int8[len]; nameInto(rtti, out); heap String(#out, len)`.
void __cajeta_rtti_name_into(void* rtti, void* out) {
    if (!out) return;
    const char* n = rtti ? ((CajetaRtti*) rtti)->typeName : "";
    if (!n) n = "";
    int64_t cap = *((int64_t*) out);
    int64_t len = (int64_t) strlen(n);
    if (len > cap) len = cap;
    if (len > 0) memcpy((char*) out + 8, n, (size_t) len);
}
// Copy the declared field name at `idx` into a caller-allocated int8[].
void __cajeta_rtti_field_name_into(void* rtti, int32_t idx, void* out) {
    if (!out) return;
    const char* n = "";
    if (rtti) {
        CajetaRtti* r = (CajetaRtti*) rtti;
        if (idx >= 0 && idx < r->propertyCount && r->properties)
            n = r->properties[idx].name ? r->properties[idx].name : "";
    }
    int64_t cap = *((int64_t*) out);
    int64_t len = (int64_t) strlen(n);
    if (len > cap) len = cap;
    if (len > 0) memcpy((char*) out + 8, n, (size_t) len);
}
int32_t __cajeta_rtti_field_name_len(void* rtti, int32_t idx) {
    if (!rtti) return 0;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->propertyCount || !r->properties) return 0;
    const char* n = r->properties[idx].name;
    return n ? (int32_t) strlen(n) : 0;
}
int32_t __cajeta_rtti_field_modifiers(void* rtti, int32_t idx) {
    if (!rtti) return 0;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->propertyCount || !r->properties) return 0;
    return r->properties[idx].modifiers;
}
int32_t __cajeta_rtti_method_modifiers(void* rtti, int32_t idx) {
    if (!rtti) return 0;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->methodCount || !r->methods) return 0;
    return r->methods[idx].modifiers;
}
int32_t __cajeta_rtti_constructor_modifiers(void* rtti, int32_t idx) {
    if (!rtti) return 0;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->constructorCount || !r->constructors) return 0;
    return r->constructors[idx].modifiers;
}

// REFL-3.3 (decision D1): reflective access is DEFAULT-OPEN, but a `@Sealed`
// class bars access to its PRIVATE members. These helpers fold "is the owning
// class sealed AND is this member private" into one boolean the reflect API
// (Field/Method/Constructor) checks before reading/writing/invoking — it throws
// IllegalAccessException when set. The modifier bits mirror cajeta.type.Modifier
// (PRIVATE=0x04) and the synthesized class modifier SEALED=0x100.
#define CAJETA_MOD_PRIVATE 0x04
#define CAJETA_MOD_SEALED  0x100
static int32_t cajeta_reflect_blocked(int32_t classMods, int32_t memberMods) {
    return ((classMods & CAJETA_MOD_SEALED) && (memberMods & CAJETA_MOD_PRIVATE))
        ? 1 : 0;
}
int32_t __cajeta_reflect_field_blocked(void* rtti, int32_t idx) {
    if (!rtti) return 0;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->propertyCount || !r->properties) return 0;
    return cajeta_reflect_blocked(r->modifiers, r->properties[idx].modifiers);
}
int32_t __cajeta_reflect_method_blocked(void* rtti, int32_t idx) {
    if (!rtti) return 0;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->methodCount || !r->methods) return 0;
    return cajeta_reflect_blocked(r->modifiers, r->methods[idx].modifiers);
}
int32_t __cajeta_reflect_ctor_blocked(void* rtti, int32_t idx) {
    if (!rtti) return 0;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->constructorCount || !r->constructors) return 0;
    return cajeta_reflect_blocked(r->modifiers, r->constructors[idx].modifiers);
}
// Byte offset of field `idx` within the instance struct (-1 if static / out of
// range). The data-driven hook reflective field get/set keys off — see
// StructureMetadata getFieldStructType / emitFieldTable.
int32_t __cajeta_rtti_field_offset(void* rtti, int32_t idx) {
    if (!rtti) return -1;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->propertyCount || !r->properties) return -1;
    return r->properties[idx].byteOffset;
}
// Field `idx`'s type-flag word (CajetaType TYPE_ID: size / int-vs-float /
// signed / primitive-vs-reference bits). 0 if out of range.
int64_t __cajeta_rtti_field_type_flags(void* rtti, int32_t idx) {
    if (!rtti) return 0;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->propertyCount || !r->properties) return 0;
    return r->properties[idx].typeFlags;
}

// REFL-3 data-driven field read/write. Each resolves field `idx`'s byteOffset
// from the #FieldDesc table (REFL-2A) and loads/stores at obj+offset — no
// per-class accessor codegen. Returns -1 offset (static / out of range) ⇒
// read yields 0/null and write is a no-op. The CALLER is responsible for
// matching the field's type (typed accessors); a size mismatch is unchecked
// (REFL-3.3 visibility/type enforcement is a later sub-task).
static int32_t cajeta_field_offset(void* rtti, int32_t idx) {
    if (!rtti) return -1;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->propertyCount || !r->properties) return -1;
    return r->properties[idx].byteOffset;
}
int32_t __cajeta_field_get_i32(void* obj, void* rtti, int32_t idx) {
    int32_t off = cajeta_field_offset(rtti, idx);
    if (!obj || off < 0) return 0;
    return *(int32_t*) ((char*) obj + off);
}
// W2 boxing: width-correct 8/16-bit field loads (the i32 load above would
// over-read a 1/2-byte field). Signed variants sign-extend, unsigned zero-extend
// (into the int32 return), so Field.getBoxed casts back to the exact wrapper
// type losslessly. 32-bit (uint32/char) and 64-bit (uint64) field reads reuse
// __cajeta_field_get_i32 / _i64 — same width, just reinterpreted in cajeta.
int32_t __cajeta_field_get_i8(void* obj, void* rtti, int32_t idx) {
    int32_t off = cajeta_field_offset(rtti, idx);
    if (!obj || off < 0) return 0;
    return (int32_t) *(int8_t*) ((char*) obj + off);
}
int32_t __cajeta_field_get_u8(void* obj, void* rtti, int32_t idx) {
    int32_t off = cajeta_field_offset(rtti, idx);
    if (!obj || off < 0) return 0;
    return (int32_t) *(uint8_t*) ((char*) obj + off);
}
int32_t __cajeta_field_get_i16(void* obj, void* rtti, int32_t idx) {
    int32_t off = cajeta_field_offset(rtti, idx);
    if (!obj || off < 0) return 0;
    return (int32_t) *(int16_t*) ((char*) obj + off);
}
int32_t __cajeta_field_get_u16(void* obj, void* rtti, int32_t idx) {
    int32_t off = cajeta_field_offset(rtti, idx);
    if (!obj || off < 0) return 0;
    return (int32_t) *(uint16_t*) ((char*) obj + off);
}
void __cajeta_field_set_i32(void* obj, void* rtti, int32_t idx, int32_t v) {
    int32_t off = cajeta_field_offset(rtti, idx);
    if (!obj || off < 0) return;
    *(int32_t*) ((char*) obj + off) = v;
}
int64_t __cajeta_field_get_i64(void* obj, void* rtti, int32_t idx) {
    int32_t off = cajeta_field_offset(rtti, idx);
    if (!obj || off < 0) return 0;
    return *(int64_t*) ((char*) obj + off);
}
void __cajeta_field_set_i64(void* obj, void* rtti, int32_t idx, int64_t v) {
    int32_t off = cajeta_field_offset(rtti, idx);
    if (!obj || off < 0) return;
    *(int64_t*) ((char*) obj + off) = v;
}
// boolean is a 1-byte field (i1 stored as i8). Read as 0/1.
int32_t __cajeta_field_get_bool(void* obj, void* rtti, int32_t idx) {
    int32_t off = cajeta_field_offset(rtti, idx);
    if (!obj || off < 0) return 0;
    return (*(int8_t*) ((char*) obj + off)) != 0 ? 1 : 0;
}
void __cajeta_field_set_bool(void* obj, void* rtti, int32_t idx, int32_t v) {
    int32_t off = cajeta_field_offset(rtti, idx);
    if (!obj || off < 0) return;
    *(int8_t*) ((char*) obj + off) = (int8_t) (v != 0 ? 1 : 0);
}
// float (f32) / double (f64) fields. Same byteOffset path as the integer
// accessors; the value is passed across the native boundary in its own FP ABI
// register, so no bit-casting is needed here.
float __cajeta_field_get_f32(void* obj, void* rtti, int32_t idx) {
    int32_t off = cajeta_field_offset(rtti, idx);
    if (!obj || off < 0) return 0.0f;
    return *(float*) ((char*) obj + off);
}
void __cajeta_field_set_f32(void* obj, void* rtti, int32_t idx, float v) {
    int32_t off = cajeta_field_offset(rtti, idx);
    if (!obj || off < 0) return;
    *(float*) ((char*) obj + off) = v;
}
double __cajeta_field_get_f64(void* obj, void* rtti, int32_t idx) {
    int32_t off = cajeta_field_offset(rtti, idx);
    if (!obj || off < 0) return 0.0;
    return *(double*) ((char*) obj + off);
}
void __cajeta_field_set_f64(void* obj, void* rtti, int32_t idx, double v) {
    int32_t off = cajeta_field_offset(rtti, idx);
    if (!obj || off < 0) return;
    *(double*) ((char*) obj + off) = v;
}
// Reference (object/pointer) field: a borrow of whatever the slot points to.
void* __cajeta_field_get_ref(void* obj, void* rtti, int32_t idx) {
    int32_t off = cajeta_field_offset(rtti, idx);
    if (!obj || off < 0) return NULL;
    return *(void**) ((char*) obj + off);
}
// Method `idx`'s canonical signature name length / copy (parallel to the field
// name readers). Used to locate a method by name and for diagnostics.
int32_t __cajeta_rtti_method_name_len(void* rtti, int32_t idx) {
    if (!rtti) return 0;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->methodCount || !r->methods) return 0;
    const char* n = r->methods[idx].name;
    return n ? (int32_t) strlen(n) : 0;
}
void __cajeta_rtti_method_name_into(void* rtti, int32_t idx, void* out) {
    if (!out) return;
    const char* n = "";
    if (rtti) {
        CajetaRtti* r = (CajetaRtti*) rtti;
        if (idx >= 0 && idx < r->methodCount && r->methods)
            n = r->methods[idx].name ? r->methods[idx].name : "";
    }
    int64_t cap = *((int64_t*) out);
    int64_t len = (int64_t) strlen(n);
    if (len > cap) len = cap;
    if (len > 0) memcpy((char*) out + 8, n, (size_t) len);
}
// Declared parameter count of method `idx` (-1 if out of range). Lets a caller
// pick out no-arg methods before a reflective invoke.
int32_t __cajeta_rtti_method_param_count(void* rtti, int32_t idx) {
    if (!rtti) return -1;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->methodCount || !r->methods) return -1;
    return r->methods[idx].parameterCount;
}

// REFL-2 reflective invoke (no-arg, scalar return path). Resolves `obj`'s
// per-class invoke adapter through its vtable -> #ClassObject -> #Rtti, then
// dispatches method `idx` with no arguments, returning the result widened to
// int64 (smaller scalars occupy the low bytes; pointers fit whole). Returns 0
// if obj/adapter is null or the index isn't a marshallable method.
//   vtable.classObject (offset 24) -> #ClassObject{ Class#VTable, rtti }
//   so rtti = *(classObject + 8).
int64_t __cajeta_object_invoke_scalar0(void* obj, int32_t idx) {
    if (!obj) return 0;
    void* vtable = *(void**) obj;
    if (!vtable) return 0;
    void* classObject = *(void**) ((char*) vtable + CAJETA_VTABLE_CLASSOBJECT_OFFSET);
    if (!classObject) return 0;
    void* rtti = *(void**) ((char*) classObject + 8);
    if (!rtti) return 0;
    void (*adapter)(void*, int32_t, void*, void*) =
        (void (*)(void*, int32_t, void*, void*)) ((CajetaRtti*) rtti)->invokeAdapter;
    if (!adapter) return 0;
    int64_t ret = 0;
    adapter(obj, idx, NULL, &ret);
    return ret;
}

// REFL-4 reflective invoke WITH arguments. `argArray` is a cajeta int64[]
// ({ i64 count, [i64 elems...] }) or NULL — each element is one raw user
// argument (scalars zero/sign-extended, pointers whole), in declared order.
// The per-class adapter reads them from an 8-byte-strided buffer, so we hand
// it the element region (skip the 8-byte count header). Result widened to int64.
int64_t __cajeta_object_invoke_scalar(void* obj, int32_t idx, void* argArray) {
    if (!obj) return 0;
    void* vtable = *(void**) obj;
    if (!vtable) return 0;
    void* classObject = *(void**) ((char*) vtable + CAJETA_VTABLE_CLASSOBJECT_OFFSET);
    if (!classObject) return 0;
    void* rtti = *(void**) ((char*) classObject + 8);
    if (!rtti) return 0;
    void (*adapter)(void*, int32_t, void*, void*) =
        (void (*)(void*, int32_t, void*, void*)) ((CajetaRtti*) rtti)->invokeAdapter;
    if (!adapter) return 0;
    void* args = argArray ? (void*) ((char*) argArray + 8) : NULL;
    int64_t ret = 0;
    adapter(obj, idx, args, &ret);
    return ret;
}

// REFL: invoke resolving the adapter from an EXPLICIT class RTTI rather than
// from the receiver `obj`. This is what makes STATIC methods reflectable: a
// static call has no receiver (`obj == NULL`), so the obj-derived path can't
// find the adapter. `Method` always carries its declaring class's rtti, so it
// can drive these. `obj` is passed straight to the adapter as the `this`
// receiver — NULL for statics, the instance for instance methods (the thunk
// only reads it for cases that have a leading `this`). The adapter belongs to
// the method's DECLARING class, so the index aligns even on a subclass instance.
void* __cajeta_rtti_invoke_obj(void* rtti, void* obj, int32_t idx, void* argArray) {
    if (!rtti) return NULL;
    void (*adapter)(void*, int32_t, void*, void*) =
        (void (*)(void*, int32_t, void*, void*)) ((CajetaRtti*) rtti)->invokeAdapter;
    if (!adapter) return NULL;
    void* args = argArray ? (void*) ((char*) argArray + 8) : NULL;
    void* ret = NULL;
    adapter(obj, idx, args, &ret);
    return ret;
}
int64_t __cajeta_rtti_invoke_scalar(void* rtti, void* obj, int32_t idx, void* argArray) {
    if (!rtti) return 0;
    void (*adapter)(void*, int32_t, void*, void*) =
        (void (*)(void*, int32_t, void*, void*)) ((CajetaRtti*) rtti)->invokeAdapter;
    if (!adapter) return 0;
    void* args = argArray ? (void*) ((char*) argArray + 8) : NULL;
    int64_t ret = 0;
    adapter(obj, idx, args, &ret);
    return ret;
}

// ---- @Inject runtime override registry (test-only DI substitution) ---------
