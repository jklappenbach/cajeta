// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// --- nucleo-column: Arrow C Data Interface shims (nucleo-column-spec §4) ----
// Interop is by matching the frozen Arrow C ABI — no libarrow anywhere
// (spec §1.3). Unit 1 ships the buffer-address primitive (64-byte alignment
// computation + probes); the matched ArrowSchema/ArrowArray structs, export,
// and import land in the plan's U3/U4.

// Data address of a cajeta array's ELEMENT buffer. Instance @Native shim on
// Storage<T>: `self` is the leading `this` the forwarder passes, ignored on
// the C side. Arrays arrive as their HEADER pointer ({ i64 count, data... });
// the element region starts at +8 — the same convention
// __cajeta_xpu_buffer_upload applies. Returning the header address instead
// was the U3 export bug: consumers read the count field as element 0.
int64_t __cajeta_arrow_addr(void* self, void* array) {
    (void) self;
    if (!array) return 0;
    return (int64_t)(intptr_t) ((char*) array + 8);
}

// --- The frozen Arrow C Data Interface structs (matched, not linked) --------
// Field order and widths are the ABI, stable since Arrow 1.0. Renamed with a
// Caj prefix so a consumer embedding both this runtime and libarrow never
// collides on the tag names; layout is byte-identical.
typedef struct CajArrowSchema {
    const char* format;
    const char* name;
    const char* metadata;
    int64_t flags;
    int64_t n_children;
    struct CajArrowSchema** children;
    struct CajArrowSchema* dictionary;
    void (*release)(struct CajArrowSchema*);
    void* private_data;
} CajArrowSchema;

typedef struct CajArrowArray {
    int64_t length;
    int64_t null_count;
    int64_t offset;
    int64_t n_buffers;
    int64_t n_children;
    const void** buffers;
    struct CajArrowArray** children;
    struct CajArrowArray* dictionary;
    void (*release)(struct CajArrowArray*);
    void* private_data;
} CajArrowArray;

#define CAJ_ARROW_FLAG_NULLABLE 2

// EXPORT BUNDLE ABI: the export shim returns the address of one heap block
// whose HEAD is { CajArrowSchema schema; CajArrowArray array; } — consumers
// (and the tests, which are consumers) read the two structs at offsets 0 and
// sizeof(CajArrowSchema). Tails after the head are private.
typedef struct CajArrowExportBundle {
    CajArrowSchema schema;
    CajArrowArray array;
    const void* buffers[2];   // [validity|NULL, data]
    int refs;                 // outstanding releases: schema + array
} CajArrowExportBundle;

// The bundle is raw malloc, DELIBERATELY outside the live set: it is a
// foreign-facing shell whose lifetime the Arrow release contract governs —
// each struct's release fires exactly once (the nulled callback is the
// spec's released marker), and the block frees when both have. The column's
// buffers are NEVER freed here: the export is a borrow (spec 4.4).
static void caj_arrow_bundle_unref(CajArrowExportBundle* b) {
    if (--b->refs == 0) free(b);
}
static void caj_arrow_schema_release(CajArrowSchema* s) {
    if (!s || !s->release) return;          // already-released: no-op
    CajArrowExportBundle* b = (CajArrowExportBundle*) s->private_data;
    s->release = NULL;
    caj_arrow_bundle_unref(b);
}
static void caj_arrow_array_release(CajArrowArray* a) {
    if (!a || !a->release) return;
    CajArrowExportBundle* b = (CajArrowExportBundle*) a->private_data;
    a->release = NULL;
    caj_arrow_bundle_unref(b);
}

// (kind, bits) -> Arrow format string. DType kind codes (DType.cajeta):
// 0 bool, 1 signed int, 2 unsigned int, 3 float. ONE table — the U4 import
// shares it in reverse. Static strings, so schema.format needs no ownership.
static const char* caj_arrow_format(int32_t kind, int32_t bits) {
    if (kind == 1) {
        switch (bits) { case 8: return "c"; case 16: return "s";
                        case 32: return "i"; case 64: return "l"; }
    }
    if (kind == 2) {
        switch (bits) { case 8: return "C"; case 16: return "S";
                        case 32: return "I"; case 64: return "L"; }
    }
    if (kind == 3) {
        switch (bits) { case 16: return "e"; case 32: return "f";
                        case 64: return "g"; }
    }
    return NULL;
}

// --- IMPORT half (U4): read foreign structs, peek elements, honor release --

// Reverse of caj_arrow_format: single-char format -> (kind<<8)|bits, -1 for
// anything v1 fixed-width import doesn't model ("e"/f16 excluded — no
// portable half-float peek; strings/nested are U5/deferred).
int64_t __cajeta_arrow_read_dtype(void* self, int64_t schemaAddr) {
    (void) self;
    const CajArrowSchema* s = (const CajArrowSchema*) (intptr_t) schemaAddr;
    if (!s || !s->format || !s->format[0] || s->format[1]) return -1;
    switch (s->format[0]) {
        case 'c': return (1 << 8) | 8;
        case 's': return (1 << 8) | 16;
        case 'i': return (1 << 8) | 32;
        case 'l': return (1 << 8) | 64;
        case 'C': return (2 << 8) | 8;
        case 'S': return (2 << 8) | 16;
        case 'I': return (2 << 8) | 32;
        case 'L': return (2 << 8) | 64;
        case 'f': return (3 << 8) | 32;
        case 'g': return (3 << 8) | 64;
        default:  return -1;
    }
}

int64_t __cajeta_arrow_read_length(void* self, int64_t arrayAddr) {
    (void) self;
    const CajArrowArray* a = (const CajArrowArray*) (intptr_t) arrayAddr;
    return a ? a->length : 0;
}

int64_t __cajeta_arrow_read_null_count(void* self, int64_t arrayAddr) {
    (void) self;
    const CajArrowArray* a = (const CajArrowArray*) (intptr_t) arrayAddr;
    return a ? a->null_count : 0;
}

int64_t __cajeta_arrow_read_offset(void* self, int64_t arrayAddr) {
    (void) self;
    const CajArrowArray* a = (const CajArrowArray*) (intptr_t) arrayAddr;
    return a ? a->offset : 0;
}

int64_t __cajeta_arrow_read_buffer(void* self, int64_t arrayAddr, int64_t idx) {
    (void) self;
    const CajArrowArray* a = (const CajArrowArray*) (intptr_t) arrayAddr;
    if (!a || !a->buffers || idx < 0 || idx >= a->n_buffers) return 0;
    return (int64_t) (intptr_t) a->buffers[idx];
}

// The cross-ABI ownership contract (spec 4.3): invoke the PRODUCER's release
// on both structs. The released-marker null checks make a second call a
// no-op, and the importing column's destructor runs once (live-set claim),
// so the producer sees exactly one release per struct.
void __cajeta_arrow_call_releases(void* self, int64_t schemaAddr,
                                  int64_t arrayAddr) {
    (void) self;
    CajArrowArray* a = (CajArrowArray*) (intptr_t) arrayAddr;
    CajArrowSchema* s = (CajArrowSchema*) (intptr_t) schemaAddr;
    if (a && a->release) a->release(a);
    if (s && s->release) s->release(s);
}

// Element peek for the foreign-backed column: a dtype-BLIND byte copy of
// element `i` into the column's 1-element owned staging buffer, which the
// cajeta side then reads back as a native `T` — no numeric conversion
// anywhere, so the generic get() stays cast-free (a `(T)` conversion inside
// an instance method miscompiles under the `Column<?>` wildcard
// instantiation's pointer-shaped sentinel).
void __cajeta_arrow_peek_into(void* self, int64_t src, int64_t i,
                              int32_t elemBytes, int64_t dst) {
    (void) self;
    memcpy((void*) (intptr_t) dst,
           (const char*) (intptr_t) src + (size_t) i * (size_t) elemBytes,
           (size_t) elemBytes);
}

// Export a fixed-width column over live buffers. validityAddr 0 = non-null
// column (buffers[0] NULL, flags 0); nonzero = bitmap present + NULLABLE
// flag. Returns the bundle address, 0 on an unsupported dtype.
int64_t __cajeta_arrow_export_fixed(void* self, int64_t dataAddr,
                                    int64_t length, int64_t nullCount,
                                    int64_t validityAddr,
                                    int32_t kind, int32_t bits) {
    (void) self;
    const char* fmt = caj_arrow_format(kind, bits);
    if (!fmt) return 0;
    CajArrowExportBundle* b =
        (CajArrowExportBundle*) calloc(1, sizeof(CajArrowExportBundle));
    if (!b) return 0;
    b->refs = 2;
    b->buffers[0] = (const void*) (intptr_t) validityAddr;
    b->buffers[1] = (const void*) (intptr_t) dataAddr;
    b->schema.format = fmt;
    b->schema.name = "";
    b->schema.flags = validityAddr ? CAJ_ARROW_FLAG_NULLABLE : 0;
    b->schema.release = caj_arrow_schema_release;
    b->schema.private_data = b;
    b->array.length = length;
    b->array.null_count = nullCount;
    b->array.n_buffers = 2;
    b->array.buffers = b->buffers;
    b->array.release = caj_arrow_array_release;
    b->array.private_data = b;
    return (int64_t)(intptr_t) b;
}
