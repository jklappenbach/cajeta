// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
// Drop-chain validation (CompilerModes.md § --drop-chain-validate): when on, every
// push / pop / mark_inactive checks the chain invariants and aborts. Default OFF.
static int __cajeta_drop_chain_validate_enabled = 0;

void __cajeta_set_drop_chain_validate(int enabled) {
    __cajeta_drop_chain_validate_enabled = enabled ? 1 : 0;
}

int __cajeta_get_drop_chain_validate(void) {
    return __cajeta_drop_chain_validate_enabled;
}

// Forward decl — the dumper lives below the push helpers.
int32_t __cajeta_dump_drop_chain(void);

// Report a chain invariant violation: message, chain dump, abort.
static void __cajeta_drop_chain_corruption(const char* code, const char* what) {
    fprintf(stderr,
        "cajeta: drop chain corruption detected (%s): %s\n",
        code, what);
    __cajeta_dump_drop_chain();
    abort();
}

// Push an owner onto the drop chain; the entry lives in the caller's frame.
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

// Push + arm in one call: the active byte comes from `flag`, so a formal's
// prologue needs one runtime call, not two.
void __cajeta_drop_push_flag(struct cajeta_drop_entry* e, void* obj,
                             void (*drop_fn)(void*), int64_t flag) {
    __cajeta_drop_push(e, obj, drop_fn);
    e->active = flag ? 1 : 0;
}

// Set by push_debug; the SIGABRT handler may read alloc_file/alloc_line only if set.
static int __cajeta_has_debug_entries = 0;

// Debug variant of __cajeta_drop_push: also writes the alloc-site source tags of the
// cajeta_drop_entry_debug shape. The base pop still works — shared offsets match.
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

// The debug-shape twin of __cajeta_drop_push_flag.
void __cajeta_drop_push_flag_debug(struct cajeta_drop_entry_debug* e, void* obj,
                                   void (*drop_fn)(void*),
                                   const char* alloc_file, int32_t alloc_line,
                                   int64_t flag) {
    __cajeta_drop_push_debug(e, obj, drop_fn, alloc_file, alloc_line);
    e->active = flag ? 1 : 0;
}

// Head-entry debug fields, valid only in a build where sourceTags is on.
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

// Print each entry of the per-thread drop chain to stderr, up to MAX_ENTRIES so a
// corrupt chain can't hang the abort path; returns the number dumped.
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
    if (count == 0) {
        fprintf(stderr, "  (empty)\n");
    }
    return count;
}

// SIGABRT handler: dump the drop chain, then chain to the previous handler.
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
    // `signal()` cannot distinguish SIG_DFL from a user handler, so just install.
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
    // Skip if SIGABRT already has a non-default handler: chaining in a JIT module's
    // handler crashes once that module unmaps. The host's static copy wins.
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

// SIGSEGV / SIGBUS handler (POSIX): print the faulting address, the running
// carrier/fiber and a backtrace, then re-raise. Runs on a sigaltstack.
#if !defined(_WIN32)

static struct sigaction __cajeta_prev_sigsegv;
static struct sigaction __cajeta_prev_sigbus;

// A fault while dumping used to re-enter this handler; the second one now exits.
static volatile sig_atomic_t __cajeta_in_segv_handler = 0;

static void __cajeta_segv_handler(int signo, siginfo_t* info, void* uctx) {
    if (__cajeta_in_segv_handler) {
        static const char msg[] = "cajeta: fault inside the crash handler — diagnostic cut short\n";
        (void) !write(2, msg, sizeof(msg) - 1);
        _exit(128 + signo);
    }
    __cajeta_in_segv_handler = 1;
    (void) uctx;
    const char* name = (signo == SIGBUS) ? "SIGBUS" : "SIGSEGV";
    fprintf(stderr, "\ncajeta: %s caught — fault addr %p\n",
            name, info ? info->si_addr : NULL);
    struct cajeta_carrier* c = __cajeta_my_carrier;
    struct cajeta_fiber* f = __cajeta_current_fiber;
    fprintf(stderr, "cajeta: carrier=%d fiber=%d\n",
            c ? c->carrier_id : -1, f ? f->dbg_id : 0);
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2 /*stderr*/);
    // The chain entries carry cajeta source tags — context a JIT backtrace lacks.
    __cajeta_dump_drop_chain();
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
    // Same "already installed?" guard as the SIGABRT path.
    struct sigaction cur;
    if (sigaction(SIGSEGV, NULL, &cur) == 0) {
        bool already = (cur.sa_flags & SA_SIGINFO)
            ? (cur.sa_sigaction != NULL)
            : (cur.sa_handler != SIG_DFL && cur.sa_handler != SIG_IGN
               && cur.sa_handler != NULL);
        if (already) return;
    }
    // Alternate stack, fixed 64 KiB: SIGSTKSZ is no longer a compile-time constant.
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

    // The constructor runs before main(), so an abort during stdlib load is caught.
__attribute__((constructor))
static void __cajeta_runtime_init(void) {
    __cajeta_install_sigabrt_handler();
    __cajeta_install_segv_handler();
}

// Pop the topmost entry and run its drop function if still active.
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

// Read whether this frame still holds title and release it in one step (the
// `return #= x` primitive): 1 if the entry was armed and the caller now owns it.
int8_t __cajeta_drop_take_active(struct cajeta_drop_entry* e) {
    if (e == NULL) {
        return 0;
    }
    int8_t was = e->active;
    e->active = 0;
    return was != 0 ? 1 : 0;
}

// Take (and disarm) only when the entry still describes `obj`; else answer 0.
int8_t __cajeta_drop_take_active_if(struct cajeta_drop_entry* e, void* obj) {
    if (e == NULL || e->obj != obj) {
        return 0;
    }
    int8_t was = e->active;
    e->active = 0;
    return was != 0 ? 1 : 0;
}

// The mark_inactive twin of __cajeta_drop_take_active_if.
void __cajeta_drop_mark_inactive_if(struct cajeta_drop_entry* e, void* obj) {
    if (e == NULL || e->obj != obj) return;
    e->active = 0;
}

// Mark an entry inactive — its owner moved out via `#`. It stays on the chain for
// scope-exit pop, but its drop function won't run.
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

// Runtime-owner formals: pushed armed, then set from the call's transfer-word bit.
void __cajeta_drop_set_flag(struct cajeta_drop_entry* e, int64_t flag) {
    e->active = flag ? 1 : 0;
}


// Debug-frame accessor: the lifetime signal for local `i` — 1 = active owner, 0 =
// moved out at runtime, -1 = no drop entry. `used, retain`; nothing calls it.
__attribute__((used, retain))
int8_t __cajeta_dbg_local_drop_active(void* frame, int i) {
    if (!frame) return -1;
    struct cajeta_dbg_frame* f = frame;
    if (i < 0 || i >= f->nlocals) return -1;
    void* e = f->locals[i].drop_entry;
    if (!e) return -1;
    return ((struct cajeta_drop_entry*) e)->active ? 1 : 0;
}


// Kind-tag dispatched drop for an interface value, at scope exit of every
// interface-typed local. Fat body: +0 data, +8 vtable (vtable[0] = drop_fn),
// +16 kind — OWNED_CLASS (1) drops through vtable[0], BORROWED kinds no-op.
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
}

// --- VTable: hash-based dispatch ---------------------------------------------
// A class's vtable is a sorted array of (signature-hash, function-pointer) entries.

struct cajeta_vtable_entry {
    int64_t hash;
    void* fn;
};

// FNV-1a 64-bit hash of a canonical signature; must stay in lockstep with the two
// compiler-side copies. '#' is skipped, so owning and borrowing instantiations of
// one template dispatch interchangeably.
int64_t __cajeta_signature_hash(const char* s) {
    if (!s) return 0;
    uint64_t h = 0xcbf29ce484222325ULL;     // FNV offset basis
    const char* p = s;
    while (*p) {
        uint8_t c = (uint8_t) *p;
        if (c == '#') {
            // A `#` inside an operator NAME (`operator#[]`) is identity, not a mode.
            int after_operator = (p - s) >= 8
                && strncmp(p - 8, "operator", 8) == 0;
            if (!after_operator) { p++; continue; }
        }
        h ^= c;
        h *= 0x100000001b3ULL;              // FNV prime
        p++;
    }
    return (int64_t) h;
}

// Binary-search the vtable for `hash`; NULL when absent, which faults at the call
// site rather than corrupting memory. The offsets below stay in lock-step with
// StructureMetadata::createVirtualTableType.
#define CAJETA_VTABLE_PARENT_OFFSET 8
#define CAJETA_VTABLE_DROP_FN_OFFSET 16
#define CAJETA_VTABLE_CLASSOBJECT_OFFSET 24
#define CAJETA_VTABLE_ENTRIES_OFFSET 32

// Virtual dispatch on drop: heap class locals push this instead of the static
// per-class wrapper, so `Animal a = heap Dog()` runs ~Dog(), not ~Animal().
void __cajeta_class_virtual_drop(void* instance) {
    if (!instance) return;
    // Idempotent claim: field drop and the owning local's pop both route here for
    // one address, and only the first runs the destructor + free.
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
// Fixed-layout C mirrors of the RTTI structs StructureMetadata.cpp emits; these
// MUST stay in lock-step with those LLVM struct shapes.

// Annotation argument descriptors, referenced from every owner descriptor's
// `annotations`. MUST match getAnnotationStructType / getAnnotationArgStructType.
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

// Parameter descriptor (#ParameterDesc) — the original 5-field shape.
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

// One declared template parameter. MUST match getTemplateParamStructType().
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
    // templateParams are the `<T>` declarations; templateArgs the concrete names.
    int16_t                       templateParamCount;
    const CajetaTemplateParamDesc* templateParams;
    int16_t                       templateArgCount;
    const char**                  templateArgs;
} CajetaRtti;

// Object.getClass(): obj -> its cached #ClassObject, a process-lifetime borrow.
void* __cajeta_object_get_class(void* obj) {
    if (!obj) return NULL;
    void* vtable = *(void**) obj;                 // header slot 0
    if (!vtable) return NULL;
    return *(void**) ((char*) vtable + CAJETA_VTABLE_CLASSOBJECT_OFFSET);
}

// Resolve a registered class name to its rtti: parent links are NAMES, so each
// level of an isa walk re-enters through this.
static void* cajeta_registry_rtti_for(const char* name);

// Transitive is-a by canonical name; depth-capped against a corrupt cyclic table.
static int32_t cajeta_rtti_isa_named(void* rttiP, const char* targetName,
                                     int depth) {
    if (!rttiP || depth > 32) return 0;
    CajetaRtti* r = (CajetaRtti*) rttiP;
    if (r->typeName && strcmp(r->typeName, targetName) == 0) return 1;
    for (int32_t i = 0; i < r->parentCount; ++i) {
        const char* pn = r->parentNames ? r->parentNames[i] : NULL;
        if (!pn) continue;
        if (strcmp(pn, targetName) == 0) return 1;
        if (cajeta_rtti_isa_named(cajeta_registry_rtti_for(pn), targetName,
                                  depth + 1)) {
            return 1;
        }
    }
    return 0;
}

// Does `obj`'s dynamic type match canonical name `targetName`? Null-safe.
int32_t __cajeta_instanceof_named(void* obj, const char* targetName) {
    if (!obj || !targetName) return 0;
    void* vtable = *(void**) obj;                 // header slot 0
    if ((uintptr_t) vtable < 4096) return 0;      // not a real vtable pointer
    void* classObject =
        *(void**) ((char*) vtable + CAJETA_VTABLE_CLASSOBJECT_OFFSET);
    if (!classObject) return 0;
    void* rtti = *(void**) ((char*) classObject + 8);
    return cajeta_rtti_isa_named(rtti, targetName, 0);
}

// --- cajeta.reflect class registry (REFL-8) --------------------------------
// canonical name -> cached #ClassObject, filled by per-class startup ctors, so
// Class.forName resolves with no live instance. Process-lifetime, never freed.
static struct { const char* name; void* classObject; }* g_cajeta_classes = NULL;
static int g_cajeta_class_count = 0;
static int g_cajeta_class_cap   = 0;

// CAJETA_REFLECT_TRACE=1: stderr trace of registrations and adapter dispatches.
static int cajeta_reflect_trace(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("CAJETA_REFLECT_TRACE");
        v = (e && *e == '1') ? 1 : 0;
    }
    return v;
}

static void* cajeta_registry_rtti_for(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < g_cajeta_class_count; ++i) {
        if (g_cajeta_classes[i].name
                && strcmp(g_cajeta_classes[i].name, name) == 0) {
            void* co = g_cajeta_classes[i].classObject;
            return co ? *(void**) ((char*) co + 8) : NULL;
        }
    }
    return NULL;
}

// Register `classObject` under canonical `name`; a duplicate takes the last writer.
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
    // strdup: a JIT'd ctor's name global can be torn down, leaving a dangling key.
    g_cajeta_classes[g_cajeta_class_count].name = strdup(name);
    g_cajeta_classes[g_cajeta_class_count].classObject = classObject;
    ++g_cajeta_class_count;
    if (cajeta_reflect_trace())
        fprintf(stderr, "[refl] reg %d %s co=%p\n",
                g_cajeta_class_count, name, classObject);
}

// Class.forName backend: `nameBytes` is a cajeta int8[] holding the canonical name.
void* __cajeta_class_for_name(void* nameBytes) {
    if (!nameBytes) return NULL;
    int64_t len = *((int64_t*) nameBytes);              // the i64 count header
    if (len < 0) return NULL;
    const char* data = (const char*) nameBytes + 8;
    for (int i = 0; i < g_cajeta_class_count; ++i) {
        const char* n = g_cajeta_classes[i].name;
        if (n && (int64_t) strlen(n) == len && memcmp(n, data, (size_t) len) == 0) {
            if (cajeta_reflect_trace())
                fprintf(stderr, "[refl] forName hit %s co=%p (of %d)\n",
                        n, g_cajeta_classes[i].classObject, g_cajeta_class_count);
            return g_cajeta_classes[i].classObject;
        }
    }
    if (cajeta_reflect_trace())
        fprintf(stderr, "[refl] forName MISS %.*s (of %d)\n",
                (int) len, data, g_cajeta_class_count);
    return NULL;
}

// Canonical name -> that type's CajetaRtti*, through the REFL-8 table. A NULL means
// "cannot prove", never a match.
#define CAJETA_CLASSOBJECT_RTTI_OFFSET 8
// `used, retain`: the debugger's parent-chain walk is the only caller.
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

// Registry enumeration for Class.allClasses / classesInPackage / classesAnnotated.
int32_t __cajeta_class_count(void) {
    return (int32_t) g_cajeta_class_count;
}
void* __cajeta_class_at(int32_t idx) {
    if (idx < 0 || idx >= g_cajeta_class_count) return NULL;
    return g_cajeta_classes[idx].classObject;
}

// Is `leafRtti`'s type the same as, or a descendant of, `boundRtti`'s? Walks the
// leaf's vtable parent chain (CAJETA_VTABLE_PARENT_OFFSET) for the bound's vtable.
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

// The same hop, exported: a debugger holds only an ADDRESS and needs the dynamic type.
__attribute__((used, retain))
void* __cajeta_rtti_of(void* obj);

// obj -> its CajetaRtti*, or NULL when obj carries no vtable/classObject/rtti.
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

// True iff canonical name `typeName` has erased base exactly `baseName`.
static int cajeta_base_name_matches(const char* typeName, const char* baseName) {
    size_t bl = strlen(baseName);
    if (strncmp(typeName, baseName, bl) != 0) return 0;
    char after = typeName[bl];
    return after == '<' || after == '\0';
}

// The last '.'-separated component of a canonical name.
static const char* cajeta_last_name(const char* s) {
    const char* dot = strrchr(s, '.');
    return dot ? dot + 1 : s;
}

// Numeric-marker code for a bound's name: 1=Numeric, 2=Floating, 3=Integral,
// 4=Complex; -1 when it is not a cajeta.lang numeric marker.
static int cajeta_numeric_marker_code(const char* boundName) {
    const char* n = cajeta_last_name(boundName);
    if (strcmp(n, "Numeric")  == 0) return 1;
    if (strcmp(n, "Floating") == 0) return 2;
    if (strcmp(n, "Integral") == 0) return 3;
    if (strcmp(n, "Complex")  == 0) return 4;
    return -1;
}

// Numeric kind of a reified element TYPE NAME: 0=bool, 1=integral, 2=float,
// 3=complex, -1 if not a numeric primitive (they carry no class RTTI to walk).
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

// Does an element of numeric kind `elemKind` satisfy marker `markerCode`?
static int cajeta_numeric_conforms(int elemKind, int markerCode) {
    if (elemKind == 0) return 0;                                  // bool: none
    if (markerCode == 1) return elemKind == 1 || elemKind == 2 || elemKind == 3;
    if (markerCode == 2) return elemKind == 2;
    if (markerCode == 3) return elemKind == 1;
    if (markerCode == 4) return elemKind == 3;
    return 0;
}

// Class-bounded-wildcard reified match: is `obj` a `baseName<...>` whose reified arg
// at `argIndex` conforms to `boundName`? Returns 0 rather than admit a mis-bound.
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
    int markerCode = cajeta_numeric_marker_code(boundName);
    if (markerCode >= 0) {
        int elemKind = cajeta_numeric_kind_of(elemName);
        if (elemKind >= 0) return cajeta_numeric_conforms(elemKind, markerCode);
        // A class element under a numeric marker falls through to the nominal walk.
    }
    void* elemRtti = __cajeta_rtti_for_name(elemName);
    void* boundRtti = __cajeta_rtti_for_name(boundName);
    if (!elemRtti || !boundRtti) return 0;                 // unresolved: fail safe
    return __cajeta_is_subtype(elemRtti, boundRtti);
}

// RTTI scalar readers — `rtti` is a CajetaRtti* (a Class instance's `rtti` field).
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

// The raw C string, for a debugger; the name_into form below writes a cajeta int8[].
__attribute__((used, retain))
const char* __cajeta_rtti_type_name(void* rtti) {
    if (!rtti) return "";
    const char* n = ((CajetaRtti*) rtti)->typeName;
    return n ? n : "";
}

__attribute__((used, retain))
const char* __cajeta_rtti_field_name(void* rtti, int32_t idx) {
    if (!rtti) return "";
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->propertyCount || !r->properties) return "";
    const char* n = r->properties[idx].name;
    return n ? n : "";
}

// A class's RTTI carries only its OWN fields, and parents are recorded as NAMES: a
// debugger walks parent_name -> __cajeta_rtti_for_name -> that RTTI's fields.
__attribute__((used, retain))
const char* __cajeta_rtti_parent_name(void* rtti, int32_t idx) {
    if (!rtti) return "";
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->parentCount || !r->parentNames) return "";
    const char* n = r->parentNames[idx];
    return n ? n : "";
}
// Copy the canonical type name into a caller-allocated int8[] ({ i64 count, bytes }),
// clamped to capacity. An out-param because a `@Native` int8[] return isn't tracked.
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

// Reflective access is DEFAULT-OPEN, but a `@Sealed` class bars its PRIVATE members.
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
// Byte offset of field `idx` in the instance struct; -1 if static or out of range.
int32_t __cajeta_rtti_field_offset(void* rtti, int32_t idx) {
    if (!rtti) return -1;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->propertyCount || !r->properties) return -1;
    return r->properties[idx].byteOffset;
}
// Field `idx`'s type-flag word (size / int-vs-float / signed / primitive bits).
int64_t __cajeta_rtti_field_type_flags(void* rtti, int32_t idx) {
    if (!rtti) return 0;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->propertyCount || !r->properties) return 0;
    return r->properties[idx].typeFlags;
}

// Data-driven field read/write: resolve field `idx`'s byteOffset, load/store at
// obj+offset. A -1 offset reads 0 and writes nothing; the CALLER matches the type.
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
// Width-correct 8/16-bit loads: signed sign-extend, unsigned zero-extend into int32.
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
// float (f32) / double (f64) fields: same byteOffset path, value in an FP register.
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
// Borrow-read the reference field at `idx` of `obj`, with the rtti derived from
// obj's own vtable (one-object provenance: a legal borrow return in cajeta).
// Defined in cajeta_rt_inject.c, included after this file: kind 6 = reference.
int32_t __cajeta_rtti_field_kind(void* rtti, int32_t idx);

static void* cajeta_object_rtti(void* obj) {
    if (!obj) return NULL;
    void* vtable = *(void**) obj;
    if (!vtable) return NULL;
    void* classObject =
        *(void**) ((char*) vtable + CAJETA_VTABLE_CLASSOBJECT_OFFSET);
    if (!classObject) return NULL;
    return *(void**) ((char*) classObject + 8);
}
void* __cajeta_object_field_ref(void* obj, int32_t idx) {
    return __cajeta_field_get_ref(obj, cajeta_object_rtti(obj), idx);
}
// Object-receiver companions for the self-walk, rtti derived from the object.
int32_t __cajeta_object_field_count(void* obj) {
    return __cajeta_rtti_field_count(cajeta_object_rtti(obj));
}
int32_t __cajeta_object_field_is_ref(void* obj, int32_t idx) {
    return __cajeta_rtti_field_kind(cajeta_object_rtti(obj), idx) == 6 ? 1 : 0;
}
int32_t __cajeta_object_field_name_len(void* obj, int32_t idx) {
    return __cajeta_rtti_field_name_len(cajeta_object_rtti(obj), idx);
}
void __cajeta_object_field_name_into(void* obj, int32_t idx, void* out) {
    __cajeta_rtti_field_name_into(cajeta_object_rtti(obj), idx, out);
}
// Method `idx`'s canonical signature name: length, then copy into an int8[].
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
// Declared parameter count of method `idx`, -1 if out of range.
int32_t __cajeta_rtti_method_param_count(void* rtti, int32_t idx) {
    if (!rtti) return -1;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->methodCount || !r->methods) return -1;
    return r->methods[idx].parameterCount;
}

// Reflective invoke (no-arg, scalar return): resolve `obj`'s per-class adapter via
// vtable -> classObject(+24) -> rtti(+8) and dispatch method `idx`.
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
    if (cajeta_reflect_trace())
        fprintf(stderr, "[refl] invoke0 type=%s idx=%d mcount=%d adapter=%p\n",
                ((CajetaRtti*) rtti)->typeName, idx,
                ((CajetaRtti*) rtti)->methodCount, (void*) adapter);
    int64_t ret = 0;
    adapter(obj, idx, NULL, &ret);
    return ret;
}

// Reflective invoke WITH arguments. `argArray` is a cajeta int64[] of raw arguments
// in declared order; the adapter wants the 8-byte-strided region past the header.
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
    if (cajeta_reflect_trace())
        fprintf(stderr, "[refl] invokeN type=%s idx=%d mcount=%d adapter=%p\n",
                ((CajetaRtti*) rtti)->typeName, idx,
                ((CajetaRtti*) rtti)->methodCount, (void*) adapter);
    void* args = argArray ? (void*) ((char*) argArray + 8) : NULL;
    int64_t ret = 0;
    adapter(obj, idx, args, &ret);
    return ret;
}

// Invoke resolving the adapter from an EXPLICIT class rtti — what makes STATIC
// methods (obj == NULL) reflectable. The adapter is the DECLARING class's.
void* __cajeta_rtti_invoke_obj(void* rtti, void* obj, int32_t idx, void* argArray) {
    if (!rtti) return NULL;
    void (*adapter)(void*, int32_t, void*, void*) =
        (void (*)(void*, int32_t, void*, void*)) ((CajetaRtti*) rtti)->invokeAdapter;
    if (!adapter) return NULL;
    if (cajeta_reflect_trace())
        fprintf(stderr, "[refl] rttiInvokeObj type=%s idx=%d mcount=%d adapter=%p\n",
                ((CajetaRtti*) rtti)->typeName, idx,
                ((CajetaRtti*) rtti)->methodCount, (void*) adapter);
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
    if (cajeta_reflect_trace())
        fprintf(stderr, "[refl] rttiInvokeScalar type=%s idx=%d mcount=%d adapter=%p\n",
                ((CajetaRtti*) rtti)->typeName, idx,
                ((CajetaRtti*) rtti)->methodCount, (void*) adapter);
    void* args = argArray ? (void*) ((char*) argArray + 8) : NULL;
    int64_t ret = 0;
    adapter(obj, idx, args, &ret);
    return ret;
}


// ---- slices 9.2.1 — local class-element array element ownership ------------
// One chain entry per owning String-element array local, pushed in the DECLARING
// frame with this stack sidecar as its obj. Ownership is tracked per SLOT.
typedef struct {
    void** arr_slot;                        // local's alloca (holds header ptr)
    struct cajeta_drop_entry* storage_entry; // the free_array entry: inactive
                                             // => array moved away, elements
                                             // travel with it (leak, not UAF)
    uint8_t* owned_bits;                    // lazily malloc'd, 1 bit per slot
    int64_t owned_cap;                      // bitmap capacity in SLOTS
    int64_t stride;                         // element slot stride (DataLayout)
    int64_t header;                         // data-region offset (DataLayout)
} cajeta_string_array_sidecar;

static int64_t caj_arr_count_masked(void* arr) {
    return *(int64_t*) arr & ~((int64_t) 1 << 63);   // CAJETA_SHARED_BIT
}

static int caj_arr_bit_get(cajeta_string_array_sidecar* sc, int64_t idx) {
    if (!sc->owned_bits || idx < 0 || idx >= sc->owned_cap) return 0;
    return (sc->owned_bits[idx >> 3] >> (idx & 7)) & 1;
}

static void caj_arr_bit_put(cajeta_string_array_sidecar* sc, int64_t idx,
                            int set) {
    if (idx < 0) return;
    if (!sc->owned_bits) {
        if (!set) return;                    // nothing marked yet, clear = no-op
        void* arr = sc->arr_slot ? *sc->arr_slot : NULL;
        if (!arr) return;
        int64_t cap = caj_arr_count_masked(arr);
        if (idx >= cap) cap = idx + 1;       // defensive; slots are bounds-checked upstream
        sc->owned_bits = (uint8_t*) calloc((size_t) ((cap + 7) >> 3), 1);
        if (!sc->owned_bits) return;         // OOM: degrade to today's leak
        sc->owned_cap = cap;
    }
    if (idx >= sc->owned_cap) return;
    if (set) sc->owned_bits[idx >> 3] |= (uint8_t) (1 << (idx & 7));
    else sc->owned_bits[idx >> 3] &= (uint8_t) ~(1 << (idx & 7));
}

// Derive a slot's element index from its address: re-evaluating the index
// expression would re-run its side effects.
static int64_t caj_arr_slot_index(cajeta_string_array_sidecar* sc,
                                  void** slot) {
    void* arr = sc->arr_slot ? *sc->arr_slot : NULL;
    if (!arr || sc->stride <= 0) return -1;
    return ((char*) slot - ((char*) arr + sc->header)) / sc->stride;
}

// Store whose source ownership the array TAKES: release the occupant, mark the slot.
void __cajeta_string_array_elem_set_owned(void* sidecar, void** slot,
                                          void* wrapper) {
    cajeta_string_array_sidecar* sc = (cajeta_string_array_sidecar*) sidecar;
    if (!sc || !slot) return;
    int64_t idx = caj_arr_slot_index(sc, slot);
    void* old = *slot;
    if (old && old != wrapper && caj_arr_bit_get(sc, idx)) {
        __cajeta_string_drop(old);
    }
    caj_arr_bit_put(sc, idx, 1);
    *slot = wrapper;
}

// Copy store (field read, literal, borrow-returning call): the source keeps its
// wrapper and the slot stores a RESOLVED fresh one it owns, so the slot cannot
// dangle when the source's owner drops.
void __cajeta_string_array_elem_set_alias(void* sidecar, void** slot,
                                          void* wrapper) {
    cajeta_string_array_sidecar* sc = (cajeta_string_array_sidecar*) sidecar;
    if (!sc || !slot) return;
    void* __cajeta_string_resolve(void* src_v);
    void* fresh = __cajeta_string_resolve(wrapper);
    int64_t idx = caj_arr_slot_index(sc, slot);
    void* old = *slot;
    if (old && old != fresh && caj_arr_bit_get(sc, idx)) {
        __cajeta_string_drop(old);
    }
    caj_arr_bit_put(sc, idx, fresh ? 1 : 0);
    *slot = fresh;
}

// Slot store for String-element arrays WITHOUT a local sidecar (field-held and
// parameter arrays): `takes` sources hand their wrapper over, others store a
// resolved copy, and the displaced occupant drops (claim-gated).
void __cajeta_string_elem_store(void** slot, void* wrapper, int64_t takes) {
    if (!slot) return;
    void* __cajeta_string_resolve(void* src_v);
    void* v = takes ? wrapper : __cajeta_string_resolve(wrapper);
    void* old = *slot;
    *slot = v;
    if (old && old != v) {
        __cajeta_string_drop(old);
    }
}

// An array literal stores every element as owned, so mark all `count` resident slots as the array's.
void __cajeta_string_array_sidecar_mark_all(void* sidecar, int64_t count) {
    cajeta_string_array_sidecar* sc = (cajeta_string_array_sidecar*) sidecar;
    if (!sc) return;
    for (int64_t i = 0; i < count; ++i) caj_arr_bit_put(sc, i, 1);
}

// `#arr[i]` — move an element OUT: hand the wrapper over, null the slot, unmark.
void* __cajeta_string_array_elem_take(void* sidecar, void** slot) {
    if (!slot) return NULL;
    void* v = *slot;
    *slot = NULL;
    if (sidecar) {
        cajeta_string_array_sidecar* sc = (cajeta_string_array_sidecar*) sidecar;
        caj_arr_bit_put(sc, caj_arr_slot_index(sc, slot), 0);
    }
    return v;
}

// Sidecar drop fn, pushed after the local's free_array entry so LIFO runs it first.
void __cajeta_string_array_owned_drop(void* sidecar) {
    cajeta_string_array_sidecar* sc = (cajeta_string_array_sidecar*) sidecar;
    if (!sc) return;
    uint8_t* bits = sc->owned_bits;
    sc->owned_bits = NULL;
    if (!bits) return;
    // Array moved away (`#arr`): its elements travel with it, and the storage
    // entry's active flag is the move-out signal.
    void* arr = sc->arr_slot ? *sc->arr_slot : NULL;
    if (arr && sc->storage_entry && sc->storage_entry->active) {
        int64_t count = caj_arr_count_masked(arr);
        if (count > sc->owned_cap) count = sc->owned_cap;
        char* data = (char*) arr + sc->header;
        for (int64_t i = 0; i < count; i++) {
            if (!((bits[i >> 3] >> (i & 7)) & 1)) continue;
            void* elem = *(void**) (data + i * sc->stride);
            if (elem) __cajeta_string_drop(elem);
        }
    }
    free(bits);
}

// ---- class-element array slot bits -----------------------------------------
// Sidecar + bitmap with TITLE semantics: a plain store is a BORROW, `a[i] = #x`
// marks the slot and releases any owned occupant, `#a[i]` is NULL when unmarked.

void __cajeta_class_array_elem_set_owned(void* sidecar, void** slot,
                                         void* obj) {
    cajeta_string_array_sidecar* sc = (cajeta_string_array_sidecar*) sidecar;
    if (!sc || !slot) return;
    int64_t idx = caj_arr_slot_index(sc, slot);
    void* old = *slot;
    if (old && old != obj && caj_arr_bit_get(sc, idx)) {
        __cajeta_class_virtual_drop(old);
    }
    caj_arr_bit_put(sc, idx, 1);
    *slot = obj;
}

void __cajeta_class_array_elem_set_alias(void* sidecar, void** slot,
                                         void* obj) {
    cajeta_string_array_sidecar* sc = (cajeta_string_array_sidecar*) sidecar;
    if (!sc || !slot) return;
    int64_t idx = caj_arr_slot_index(sc, slot);
    void* old = *slot;
    if (old && old != obj && caj_arr_bit_get(sc, idx)) {
        __cajeta_class_virtual_drop(old);
    }
    caj_arr_bit_put(sc, idx, (old == obj && old) ? caj_arr_bit_get(sc, idx) : 0);
    *slot = obj;
}

void* __cajeta_class_array_elem_take(void* sidecar, void** slot) {
    cajeta_string_array_sidecar* sc = (cajeta_string_array_sidecar*) sidecar;
    if (!sc || !slot) return NULL;
    int64_t idx = caj_arr_slot_index(sc, slot);
    if (!caj_arr_bit_get(sc, idx)) return NULL;   // borrowed/empty: no title
    void* v = *slot;
    if (!v) return NULL;
    caj_arr_bit_put(sc, idx, 0);                  // title out; slot stays
    return v;
}

// ===== title-stores §3 — tail-bitmap element titles ========================
// Drop-entry shape (one arg) walking a class-pointer array's tail bitmap; those
// arrays are always {i64 count | ptr data[] | bits}, so header and stride are 8.
void __cajeta_tail_array_drop(void* hdr);

// Arrays from __cajeta_new_array_header_bits carry a per-slot ownership bitmap at
// hdr + header_size + count*elem_size, so field arrays and locals ride one mechanism.

static uint8_t* caj_tail_bits(void* hdr, uint64_t header_size, uint64_t elem_size) {
    int64_t count = *(int64_t*) hdr & ~((int64_t) 1 << 63);
    return (uint8_t*) hdr + header_size + (uint64_t) count * elem_size;
}

// Store with displaced release: an OWNED occupant drops before the overwrite.
void __cajeta_tail_elem_store(void* hdr, uint64_t header_size, uint64_t elem_size,
                              int64_t idx, void* obj, int64_t owned) {
    if (!hdr) return;
    int64_t count = *(int64_t*) hdr & ~((int64_t) 1 << 63);
    if (idx < 0 || idx >= count) return;    // bounds guarded upstream; stay safe
    uint8_t* bits = caj_tail_bits(hdr, header_size, elem_size);
    void** slot = (void**) ((uint8_t*) hdr + header_size + (uint64_t) idx * elem_size);
    void* old = *slot;
    if (old && old != obj && ((bits[idx >> 3] >> (idx & 7)) & 1)) {
        __cajeta_class_virtual_drop(old);
    }
    // Storing a borrow of the slot's OWN value back over it changes no hands: the slot keeps its title.
    if (old == obj && old && !(owned & 1)) owned = (bits[idx >> 3] >> (idx & 7)) & 1;
    if (owned & 1) bits[idx >> 3] |= (uint8_t) (1 << (idx & 7));
    else           bits[idx >> 3] &= (uint8_t) ~(1 << (idx & 7));
    *slot = obj;
}

// Move-out: clear the slot's bit and return its previous value (1 = caller has title).
int64_t __cajeta_tail_elem_take_flag(void* hdr, uint64_t header_size,
                                     uint64_t elem_size, int64_t idx) {
    if (!hdr) return 0;
    int64_t count = *(int64_t*) hdr & ~((int64_t) 1 << 63);
    if (idx < 0 || idx >= count) return 0;
    uint8_t* bits = caj_tail_bits(hdr, header_size, elem_size);
    int64_t was = (bits[idx >> 3] >> (idx & 7)) & 1;
    bits[idx >> 3] &= (uint8_t) ~(1 << (idx & 7));
    return was;
}

// Bit-guarded single drop: owned -> drop, clear, null the slot; borrowed -> no-op.
void __cajeta_tail_elem_drop_one(void* hdr, uint64_t header_size,
                                 uint64_t elem_size, int64_t idx) {
    if (!hdr) return;
    int64_t count = *(int64_t*) hdr & ~((int64_t) 1 << 63);
    if (idx < 0 || idx >= count) return;
    uint8_t* bits = caj_tail_bits(hdr, header_size, elem_size);
    if (!((bits[idx >> 3] >> (idx & 7)) & 1)) return;
    void** slot = (void**) ((uint8_t*) hdr + header_size + (uint64_t) idx * elem_size);
    void* v = *slot;
    bits[idx >> 3] &= (uint8_t) ~(1 << (idx & 7));
    *slot = NULL;
    if (v) __cajeta_class_virtual_drop(v);
}

// Teardown walk: drop every OWNED slot, clearing as it goes; bit-0 slots untouched.
void __cajeta_tail_elem_drop_walk(void* hdr, uint64_t header_size,
                                  uint64_t elem_size);

void __cajeta_tail_array_drop(void* hdr) {
    __cajeta_tail_elem_drop_walk(hdr, 8, 8);
}

// Single-entry local-array drop, so a move-out deactivates walk and free together.
void __cajeta_free_array(void* array);
void __cajeta_tail_array_drop_free(void* hdr) {
    __cajeta_tail_elem_drop_walk(hdr, 8, 8);
    __cajeta_free_array(hdr);
}

// JAGGED arrays: an ARRAY element is droppable but has no vtable to drop through.
// `inner_kind` is 0 = plain free, 1 = inner array has its own tail bitmap.
void __cajeta_free_array(void* array);

static void caj_arrelem_release(void* v, int64_t inner_kind) {
    if (v == NULL) return;
    if (inner_kind == 1) { __cajeta_tail_array_drop_free(v); return; }
    __cajeta_free_array(v);
}

// Store with displaced release — the array-element twin of __cajeta_tail_elem_store.
void __cajeta_tail_arrelem_store(void* hdr, uint64_t header_size,
                                 uint64_t elem_size, int64_t idx, void* obj,
                                 int64_t owned, int64_t inner_kind) {
    if (!hdr) return;
    int64_t count = *(int64_t*) hdr & ~((int64_t) 1 << 63);
    if (idx < 0 || idx >= count) return;
    uint8_t* bits = caj_tail_bits(hdr, header_size, elem_size);
    void** slot = (void**) ((uint8_t*) hdr + header_size + (uint64_t) idx * elem_size);
    void* old = *slot;
    if (old && old != obj && ((bits[idx >> 3] >> (idx & 7)) & 1)) {
        caj_arrelem_release(old, inner_kind);
    }
    // Storing a borrow of the slot's OWN value back over it changes no hands: the slot keeps its title.
    if (old == obj && old && !(owned & 1)) owned = (bits[idx >> 3] >> (idx & 7)) & 1;
    if (owned & 1) bits[idx >> 3] |= (uint8_t) (1 << (idx & 7));
    else           bits[idx >> 3] &= (uint8_t) ~(1 << (idx & 7));
    *slot = obj;
}

// Bit-guarded single drop — the array-element twin of __cajeta_tail_elem_drop_one.
void __cajeta_tail_arrelem_drop_one(void* hdr, uint64_t header_size,
                                    uint64_t elem_size, int64_t idx,
                                    int64_t inner_kind) {
    if (!hdr) return;
    int64_t count = *(int64_t*) hdr & ~((int64_t) 1 << 63);
    if (idx < 0 || idx >= count) return;
    uint8_t* bits = caj_tail_bits(hdr, header_size, elem_size);
    if (!((bits[idx >> 3] >> (idx & 7)) & 1)) return;
    void** slot = (void**) ((uint8_t*) hdr + header_size + (uint64_t) idx * elem_size);
    void* v = *slot;
    bits[idx >> 3] &= (uint8_t) ~(1 << (idx & 7));
    *slot = NULL;
    if (v) caj_arrelem_release(v, inner_kind);
}

// Teardown walk — the array-element twin of __cajeta_tail_elem_drop_walk.
void __cajeta_tail_arrelem_drop_walk(void* hdr, uint64_t header_size,
                                     uint64_t elem_size, int64_t inner_kind) {
    if (!hdr) return;
    int64_t count = *(int64_t*) hdr & ~((int64_t) 1 << 63);
    uint8_t* bits = caj_tail_bits(hdr, header_size, elem_size);
    for (int64_t i = 0; i < count; i++) {
        if (!((bits[i >> 3] >> (i & 7)) & 1)) continue;
        void** slot = (void**) ((uint8_t*) hdr + header_size + (uint64_t) i * elem_size);
        void* v = *slot;
        bits[i >> 3] &= (uint8_t) ~(1 << (i & 7));
        *slot = NULL;
        if (v) caj_arrelem_release(v, inner_kind);
    }
}

// String-element arrays: every RESIDENT slot owns its wrapper, so the teardown
// walk releases unconditionally; taken slots hold NULL.
void __cajeta_string_elem_drop_walk(void* hdr, uint64_t header_size,
                                    uint64_t elem_size) {
    if (!hdr) return;
    int64_t scount = *(int64_t*) hdr & ~((int64_t) 1 << 63);
    for (int64_t i = 0; i < scount; i++) {
        void** slot = (void**) ((uint8_t*) hdr + header_size + (uint64_t) i * elem_size);
        void* v = *slot;
        *slot = NULL;
        if (v) __cajeta_class_virtual_drop(v);
    }
}

void __cajeta_tail_elem_drop_walk(void* hdr, uint64_t header_size,
                                  uint64_t elem_size) {
    if (!hdr) return;
    int64_t count = *(int64_t*) hdr & ~((int64_t) 1 << 63);
    uint8_t* bits = caj_tail_bits(hdr, header_size, elem_size);
    for (int64_t i = 0; i < count; i++) {
        if (!((bits[i >> 3] >> (i & 7)) & 1)) continue;
        void** slot = (void**) ((uint8_t*) hdr + header_size + (uint64_t) i * elem_size);
        void* v = *slot;
        bits[i >> 3] &= (uint8_t) ~(1 << (i & 7));
        *slot = NULL;
        if (v) __cajeta_class_virtual_drop(v);
    }
}

void __cajeta_class_array_owned_drop(void* sidecar) {
    cajeta_string_array_sidecar* sc = (cajeta_string_array_sidecar*) sidecar;
    if (!sc) return;
    uint8_t* bits = sc->owned_bits;
    sc->owned_bits = NULL;
    if (!bits) return;
    void* arr = sc->arr_slot ? *sc->arr_slot : NULL;
    if (arr && sc->storage_entry && sc->storage_entry->active) {
        int64_t count = caj_arr_count_masked(arr);
        if (count > sc->owned_cap) count = sc->owned_cap;
        char* data = (char*) arr + sc->header;
        for (int64_t i = 0; i < count; i++) {
            if (!((bits[i >> 3] >> (i & 7)) & 1)) continue;
            void* elem = *(void**) (data + i * sc->stride);
            if (elem) __cajeta_class_virtual_drop(elem);
        }
    }
    free(bits);
}
