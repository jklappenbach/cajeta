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

struct cajeta_exception_frame {
    jmp_buf buf;
    struct cajeta_exception_frame* prev;
    int64_t thrown_value;
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
    __cajeta_exc_top->thrown_value = value;
    longjmp(__cajeta_exc_top->buf, 1);
}

int64_t __cajeta_get_thrown(void) {
    return __cajeta_exc_top ? __cajeta_exc_top->thrown_value : 0;
}
