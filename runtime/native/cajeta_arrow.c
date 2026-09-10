// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// Arrow C Data Interface shims: the frozen ABI is matched here, never linked.

// Data address of an array's ELEMENT buffer: the { i64 count, data... } header
// arrives at +0, so the elements start at +8.
int64_t __cajeta_arrow_addr(void* self, void* array) {
    (void) self;
    if (!array) return 0;
    return (int64_t)(intptr_t) ((char*) array + 8);
}

// --- The frozen Arrow structs: field order and width are the ABI -----------
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

// EXPORT BUNDLE ABI: head = { CajArrowSchema schema; CajArrowArray array; }.
typedef struct CajArrowExportBundle {
    CajArrowSchema schema;
    CajArrowArray array;
    const void* buffers[2];   // [validity|NULL, data]
    int refs;                 // outstanding releases: schema + array
} CajArrowExportBundle;

// Drops one of the two release refs; the exported buffers are a borrow.
static void caj_arrow_bundle_unref(CajArrowExportBundle* b) {
    if (--b->refs == 0) free(b);
}

// The array struct's address in the bundle at `bundleAddr` (schema is at +0).
int64_t __cajeta_arrow_bundle_array(void* self, int64_t bundleAddr) {
    (void) self;
    return bundleAddr + (int64_t) offsetof(CajArrowExportBundle, array);
}
static void caj_arrow_schema_release(CajArrowSchema* s) {
    if (!s || !s->release) return;
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

// (kind, bits) -> static format string; kind 0 bool, 1 int, 2 uint, 3 float.
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

// Reverse of caj_arrow_format: format char -> (kind<<8)|bits, -1 if unmodelled.
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

// Calls the producer's release on both structs; a nulled callback means released.
void __cajeta_arrow_call_releases(void* self, int64_t schemaAddr,
                                  int64_t arrayAddr) {
    (void) self;
    CajArrowArray* a = (CajArrowArray*) (intptr_t) arrayAddr;
    CajArrowSchema* s = (CajArrowSchema*) (intptr_t) schemaAddr;
    if (a && a->release) a->release(a);
    if (s && s->release) s->release(s);
}

// Dtype-BLIND copy of element `i` into the 1-element staging buffer: a `(T)`
// conversion would miscompile under the `Column<?>` wildcard instantiation.
void __cajeta_arrow_peek_into(void* self, int64_t src, int64_t i,
                              int32_t elemBytes, int64_t dst) {
    (void) self;
    memcpy((void*) (intptr_t) dst,
           (const char*) (intptr_t) src + (size_t) i * (size_t) elemBytes,
           (size_t) elemBytes);
}

// 1 iff the schema's format is utf8 ("u"); large utf8 ("U") is not modelled.
int64_t __cajeta_arrow_read_is_utf8(void* self, int64_t schemaAddr) {
    (void) self;
    const CajArrowSchema* s = (const CajArrowSchema*) (intptr_t) schemaAddr;
    return (s && s->format && s->format[0] == 'u' && !s->format[1]) ? 1 : 0;
}

int32_t __cajeta_arrow_peek_i32(void* self, int64_t addr, int64_t i) {
    (void) self;
    return ((const int32_t*) (intptr_t) addr)[i];
}
int32_t __cajeta_arrow_peek_u8(void* self, int64_t addr, int64_t i) {
    (void) self;
    return (int32_t) ((const uint8_t*) (intptr_t) addr)[i];
}

// utf8 export: buffers are [validity|NULL, offsets(int32, len+1), data bytes].
typedef struct CajArrowExportBundle3 {
    CajArrowSchema schema;
    CajArrowArray array;
    const void* buffers[3];
    int refs;
} CajArrowExportBundle3;

static void caj_arrow_schema_release3(CajArrowSchema* s) {
    if (!s || !s->release) return;
    CajArrowExportBundle3* b = (CajArrowExportBundle3*) s->private_data;
    s->release = NULL;
    if (--b->refs == 0) free(b);
}
static void caj_arrow_array_release3(CajArrowArray* a) {
    if (!a || !a->release) return;
    CajArrowExportBundle3* b = (CajArrowExportBundle3*) a->private_data;
    a->release = NULL;
    if (--b->refs == 0) free(b);
}

int64_t __cajeta_arrow_export_varlen(void* self, int64_t offsetsAddr,
                                     int64_t dataAddr, int64_t length) {
    (void) self;
    CajArrowExportBundle3* b =
        (CajArrowExportBundle3*) calloc(1, sizeof(CajArrowExportBundle3));
    if (!b) return 0;
    b->refs = 2;
    b->buffers[0] = NULL;
    b->buffers[1] = (const void*) (intptr_t) offsetsAddr;
    b->buffers[2] = (const void*) (intptr_t) dataAddr;
    b->schema.format = "u";
    b->schema.name = "";
    b->schema.release = caj_arrow_schema_release3;
    b->schema.private_data = b;
    b->array.length = length;
    b->array.null_count = 0;
    b->array.n_buffers = 3;
    b->array.buffers = b->buffers;
    b->array.release = caj_arrow_array_release3;
    b->array.private_data = b;
    return (int64_t)(intptr_t) b;
}

// --- MX extension types: a logical name + metadata over packed uint8 --------
// Metadata blob: int32 n_pairs, then per pair int32 keyLen, key, int32 valLen, value.

#define CAJ_MX_FP4_NAME "cajeta.mxfp4"

typedef struct CajArrowExtBundle {
    CajArrowSchema schema;
    CajArrowArray array;
    const void* buffers[2];
    int refs;
    char metadata[128];
    char extMeta[32];
} CajArrowExtBundle;

static void caj_arrow_schema_release_ext(CajArrowSchema* s) {
    if (!s || !s->release) return;
    CajArrowExtBundle* b = (CajArrowExtBundle*) s->private_data;
    s->release = NULL;
    if (--b->refs == 0) free(b);
}
static void caj_arrow_array_release_ext(CajArrowArray* a) {
    if (!a || !a->release) return;
    CajArrowExtBundle* b = (CajArrowExtBundle*) a->private_data;
    a->release = NULL;
    if (--b->refs == 0) free(b);
}

static size_t caj_meta_put(char* p, const char* key, const char* val) {
    int32_t kl = (int32_t) strlen(key);
    int32_t vl = (int32_t) strlen(val);
    size_t n = 0;
    memcpy(p + n, &kl, 4); n += 4;
    memcpy(p + n, key, (size_t) kl); n += (size_t) kl;
    memcpy(p + n, &vl, 4); n += 4;
    memcpy(p + n, val, (size_t) vl); n += (size_t) vl;
    return n;
}

// Exports packed MX bytes as uint8 ("C") tagged with the name and block size.
int64_t __cajeta_arrow_export_mx(void* self, int64_t dataAddr,
                                 int64_t byteLen, int32_t blockSize) {
    (void) self;
    CajArrowExtBundle* b =
        (CajArrowExtBundle*) calloc(1, sizeof(CajArrowExtBundle));
    if (!b) return 0;
    b->refs = 2;
    snprintf(b->extMeta, sizeof(b->extMeta), "{\"block\":%d}", blockSize);
    char* p = b->metadata;
    int32_t pairs = 2;
    memcpy(p, &pairs, 4);
    size_t off = 4;
    off += caj_meta_put(p + off, "ARROW:extension:name", CAJ_MX_FP4_NAME);
    off += caj_meta_put(p + off, "ARROW:extension:metadata", b->extMeta);
    (void) off;
    b->buffers[0] = NULL;
    b->buffers[1] = (const void*) (intptr_t) dataAddr;
    b->schema.format = "C";
    b->schema.name = "";
    b->schema.metadata = b->metadata;
    b->schema.release = caj_arrow_schema_release_ext;
    b->schema.private_data = b;
    b->array.length = byteLen;
    b->array.n_buffers = 2;
    b->array.buffers = b->buffers;
    b->array.release = caj_arrow_array_release_ext;
    b->array.private_data = b;
    return (int64_t)(intptr_t) b;
}

// ARROW:extension:name: 1 = cajeta.mxfp4, 0 = none, -1 = another extension.
int64_t __cajeta_arrow_read_ext_kind(void* self, int64_t schemaAddr) {
    (void) self;
    const CajArrowSchema* s = (const CajArrowSchema*) (intptr_t) schemaAddr;
    if (!s || !s->metadata) return 0;
    const char* p = s->metadata;
    int32_t pairs;
    memcpy(&pairs, p, 4);
    p += 4;
    for (int32_t k = 0; k < pairs; ++k) {
        int32_t kl; memcpy(&kl, p, 4); p += 4;
        const char* key = p; p += kl;
        int32_t vl; memcpy(&vl, p, 4); p += 4;
        const char* val = p; p += vl;
        if (kl == (int32_t) strlen("ARROW:extension:name")
                && memcmp(key, "ARROW:extension:name", (size_t) kl) == 0) {
            if (vl == (int32_t) strlen(CAJ_MX_FP4_NAME)
                    && memcmp(val, CAJ_MX_FP4_NAME, (size_t) vl) == 0) {
                return 1;
            }
            return -1;
        }
    }
    return 0;
}

// Block size out of the mx extension metadata ({"block":N}); 0 when absent.
int64_t __cajeta_arrow_read_ext_block(void* self, int64_t schemaAddr) {
    (void) self;
    const CajArrowSchema* s = (const CajArrowSchema*) (intptr_t) schemaAddr;
    if (!s || !s->metadata) return 0;
    const char* p = s->metadata;
    int32_t pairs;
    memcpy(&pairs, p, 4);
    p += 4;
    for (int32_t k = 0; k < pairs; ++k) {
        int32_t kl; memcpy(&kl, p, 4); p += 4;
        const char* key = p; p += kl;
        int32_t vl; memcpy(&vl, p, 4); p += 4;
        const char* val = p; p += vl;
        if (kl == (int32_t) strlen("ARROW:extension:metadata")
                && memcmp(key, "ARROW:extension:metadata", (size_t) kl) == 0) {
            long n = 0;
            for (int32_t i = 0; i < vl; ++i) {
                if (val[i] >= '0' && val[i] <= '9') n = n * 10 + (val[i] - '0');
            }
            return (int64_t) n;
        }
    }
    return 0;
}

// Exports a fixed-width column over live buffers; validityAddr 0 means non-null,
// nonzero adds the bitmap + NULLABLE flag. 0 = no Arrow format for that dtype.
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
