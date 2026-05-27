// __emutls_get_address — emulated thread-local storage helper.
//
// Why this exists in the test binary:
//
//   LLJIT lowers `thread_local` globals via clang/LLVM's TargetMachine,
//   which on some platforms (notably macOS arm64) defaults to emulated
//   TLS (emutls) — every TLS access lowers to a call to
//   `__emutls_get_address(__emutls_control*)`. The implementation lives
//   in compiler-rt's builtins, shipped as a static archive
//   (libclang_rt.builtins-<arch>.a). LLJIT's process symbol generator
//   resolves against dynamically-loaded symbols only, so the helper
//   isn't on the JIT's search path and bitcode loads fail with
//   "Symbols not found: [ ___emutls_get_address ]".
//
//   The previous workaround (forcing the LLJIT TargetMachine to
//   disable EmulatedTLS) caused a worse failure on macOS arm64: the
//   resulting real-TLS lowering emitted calls to `_tlv_bootstrap` —
//   dyld's INTERNAL TLV initializer — which aborts when called
//   directly because it expects dyld linker state. LLJIT's macOS
//   arm64 real-TLS support is incomplete; emutls is the workable
//   path.
//
//   So: link a real implementation of __emutls_get_address into the
//   TEST binary (not the runtime bitcode that gets JIT'd). The JIT's
//   DynamicLibrarySearchGenerator::GetForCurrentProcess() then finds
//   it via dlsym() against the test binary's own symbol table.
//
//   Mark the symbol weak so it doesn't conflict with compiler-rt's
//   builtins archive on platforms where ld already provides one.
//
// Algorithm (matches compiler-rt's emutls.c):
//   - First call assigns a monotonically increasing index to each
//     unique __emutls_control* (under a global mutex).
//   - Per-thread storage is a pthread-key-anchored array of pointers;
//     entry N holds the thread-local storage for the variable with
//     index N. The array grows on demand.
//   - First per-thread access to a variable allocates aligned storage,
//     initializes it from the control's `value` field (or zeros it),
//     and caches the address in the per-thread array.
//   - Subsequent accesses return the cached pointer.
//   - On thread exit, the pthread-key destructor frees every cached
//     allocation and the array itself.

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Matches compiler-rt's emutls_control struct.
typedef struct __emutls_control {
    size_t size;
    size_t align;
    union {
        uintptr_t index;
        void* address;
    } object;
    void* value;
} __emutls_control;

static pthread_mutex_t emutls_mutex = PTHREAD_MUTEX_INITIALIZER;
static size_t emutls_num_objects = 0;
static pthread_key_t emutls_pthread_key;
static pthread_once_t emutls_init_once = PTHREAD_ONCE_INIT;

static void emutls_key_destructor(void* ptr) {
    if (!ptr) return;
    void** arr = (void**) ptr;
    // arr[0] holds the current array capacity (in entries, NOT bytes).
    size_t cap = (size_t) (uintptr_t) arr[0];
    for (size_t i = 1; i <= cap; ++i) {
        if (arr[i]) free(arr[i]);
    }
    free(arr);
}

static void emutls_init(void) {
    pthread_key_create(&emutls_pthread_key, emutls_key_destructor);
}

static void* emutls_allocate_object(__emutls_control* control) {
    size_t align = control->align ? control->align : sizeof(void*);
    // Round size up to alignment so aligned_alloc is happy.
    size_t size = (control->size + align - 1) & ~(align - 1);
    void* obj;
    if (posix_memalign(&obj, align < sizeof(void*) ? sizeof(void*) : align,
                       size ? size : sizeof(void*)) != 0) {
        return NULL;
    }
    if (control->value) {
        memcpy(obj, control->value, control->size);
    } else {
        memset(obj, 0, control->size);
    }
    return obj;
}

// `used` keeps the linker from dead-stripping this symbol — it's
// referenced only by JIT-compiled bitcode at runtime, never by anything
// the linker sees at link time. `visibility("default")` ensures the
// symbol is in the dynamic-export table so the JIT's dlsym() (via
// DynamicLibrarySearchGenerator::GetForCurrentProcess) can find it.
__attribute__((weak, used, visibility("default")))
void* __emutls_get_address(__emutls_control* control) {
    pthread_once(&emutls_init_once, emutls_init);

    // Assign a global index to this variable on first call. The index
    // identifies the variable across all threads.
    uintptr_t idx = control->object.index;
    if (idx == 0) {
        pthread_mutex_lock(&emutls_mutex);
        idx = control->object.index;
        if (idx == 0) {
            idx = ++emutls_num_objects;
            control->object.index = idx;
        }
        pthread_mutex_unlock(&emutls_mutex);
    }

    // Per-thread array of pointers, indexed by emutls variable index.
    // arr[0] = capacity (so [1..cap] are storage slots).
    void** arr = (void**) pthread_getspecific(emutls_pthread_key);
    size_t cap = arr ? (size_t) (uintptr_t) arr[0] : 0;

    if (cap < idx) {
        // Grow: pick a capacity that won't need to grow again for a while.
        size_t new_cap = (idx * 3 + 2) / 2;
        if (new_cap < 16) new_cap = 16;
        void** new_arr = (void**) calloc(new_cap + 1, sizeof(void*));
        if (!new_arr) return NULL;
        if (arr) {
            memcpy(new_arr + 1, arr + 1, cap * sizeof(void*));
            free(arr);
        }
        new_arr[0] = (void*) (uintptr_t) new_cap;
        pthread_setspecific(emutls_pthread_key, new_arr);
        arr = new_arr;
    }

    if (!arr[idx]) {
        arr[idx] = emutls_allocate_object(control);
    }
    return arr[idx];
}
