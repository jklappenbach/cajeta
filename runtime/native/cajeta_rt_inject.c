// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// ---- @Inject runtime override registry (test-only DI substitution) ---------
// Binds a substitute instance for a type, keyed by `reflect.Class` object pointer, in
// test builds only. Entries are BORROWED: clear() forgets them, it never frees them.
typedef struct CajetaInjectOverride {
    void* classObj;
    void* instance;
    struct CajetaInjectOverride* next;
} CajetaInjectOverride;

static CajetaInjectOverride* __cajeta_inject_override_head = NULL;
static pthread_mutex_t __cajeta_inject_override_mutex = PTHREAD_MUTEX_INITIALIZER;

void __cajeta_inject_override_bind(void* classObj, void* instance) {
    if (!classObj) return;
    pthread_mutex_lock(&__cajeta_inject_override_mutex);
    for (CajetaInjectOverride* e = __cajeta_inject_override_head; e; e = e->next) {
        if (e->classObj == classObj) {
            e->instance = instance;
            pthread_mutex_unlock(&__cajeta_inject_override_mutex);
            return;
        }
    }
    CajetaInjectOverride* node =
        (CajetaInjectOverride*) malloc(sizeof(CajetaInjectOverride));
    if (!node) {
        pthread_mutex_unlock(&__cajeta_inject_override_mutex);
        return;
    }
    node->classObj = classObj;
    node->instance = instance;
    node->next = __cajeta_inject_override_head;
    __cajeta_inject_override_head = node;
    pthread_mutex_unlock(&__cajeta_inject_override_mutex);
}

void* __cajeta_inject_override_get(void* classObj) {
    if (!classObj) return NULL;
    void* result = NULL;
    pthread_mutex_lock(&__cajeta_inject_override_mutex);
    for (CajetaInjectOverride* e = __cajeta_inject_override_head; e; e = e->next) {
        if (e->classObj == classObj) {
            result = e->instance;
            break;
        }
    }
    pthread_mutex_unlock(&__cajeta_inject_override_mutex);
    return result;
}

void __cajeta_inject_override_clear(void) {
    pthread_mutex_lock(&__cajeta_inject_override_mutex);
    CajetaInjectOverride* e = __cajeta_inject_override_head;
    while (e) {
        CajetaInjectOverride* n = e->next;
        free(e);
        e = n;
    }
    __cajeta_inject_override_head = NULL;
    pthread_mutex_unlock(&__cajeta_inject_override_mutex);
}

// Resolves a class's reflective invoke adapter through vtable -> classObject -> rtti. The
// typed variants below read its 8-byte ret buffer in the float, double or pointer register.
static void* cajeta_resolve_invoke_adapter(void* obj) {
    if (!obj) return NULL;
    void* vtable = *(void**) obj;
    if (!vtable) return NULL;
    void* classObject = *(void**) ((char*) vtable + CAJETA_VTABLE_CLASSOBJECT_OFFSET);
    if (!classObject) return NULL;
    void* rtti = *(void**) ((char*) classObject + 8);
    if (!rtti) return NULL;
    return ((CajetaRtti*) rtti)->invokeAdapter;
}
float __cajeta_object_invoke_f32(void* obj, int32_t idx, void* argArray) {
    void (*adapter)(void*, int32_t, void*, void*) =
        (void (*)(void*, int32_t, void*, void*)) cajeta_resolve_invoke_adapter(obj);
    if (!adapter) return 0.0f;
    void* args = argArray ? (void*) ((char*) argArray + 8) : NULL;
    int64_t retBits = 0;
    adapter(obj, idx, args, &retBits);
    float out;
    memcpy(&out, &retBits, sizeof(out));
    return out;
}
double __cajeta_object_invoke_f64(void* obj, int32_t idx, void* argArray) {
    void (*adapter)(void*, int32_t, void*, void*) =
        (void (*)(void*, int32_t, void*, void*)) cajeta_resolve_invoke_adapter(obj);
    if (!adapter) return 0.0;
    void* args = argArray ? (void*) ((char*) argArray + 8) : NULL;
    double ret = 0.0;
    adapter(obj, idx, args, &ret);
    return ret;
}
// Invokes a reference-returning method; ownership transfers per that method's signature.
void* __cajeta_object_invoke_obj(void* obj, int32_t idx, void* argArray) {
    void (*adapter)(void*, int32_t, void*, void*) =
        (void (*)(void*, int32_t, void*, void*)) cajeta_resolve_invoke_adapter(obj);
    if (!adapter) return NULL;
    void* args = argArray ? (void*) ((char*) argArray + 8) : NULL;
    void* ret = NULL;
    adapter(obj, idx, args, &ret);
    return ret;
}

// Fiber-stack argument buffers: the caller passes a statically known count of discrete
// int64 args, assembled here into the adapter's buffer with no heap int64[] and no header.
int64_t __cajeta_object_invoke_scalar1(void* obj, int32_t idx, int64_t a0) {
    void (*adapter)(void*, int32_t, void*, void*) =
        (void (*)(void*, int32_t, void*, void*)) cajeta_resolve_invoke_adapter(obj);
    if (!adapter) return 0;
    int64_t buf[1] = { a0 };
    int64_t ret = 0;
    adapter(obj, idx, buf, &ret);
    return ret;
}
int64_t __cajeta_object_invoke_scalar2(void* obj, int32_t idx, int64_t a0, int64_t a1) {
    void (*adapter)(void*, int32_t, void*, void*) =
        (void (*)(void*, int32_t, void*, void*)) cajeta_resolve_invoke_adapter(obj);
    if (!adapter) return 0;
    int64_t buf[2] = { a0, a1 };
    int64_t ret = 0;
    adapter(obj, idx, buf, &ret);
    return ret;
}
int64_t __cajeta_object_invoke_scalar3(void* obj, int32_t idx,
                                       int64_t a0, int64_t a1, int64_t a2) {
    void (*adapter)(void*, int32_t, void*, void*) =
        (void (*)(void*, int32_t, void*, void*)) cajeta_resolve_invoke_adapter(obj);
    if (!adapter) return 0;
    int64_t buf[3] = { a0, a1, a2 };
    int64_t ret = 0;
    adapter(obj, idx, buf, &ret);
    return ret;
}

// One parameter descriptor; paramIdx is the USER index, `this` being absent from the table.
static const CajetaParamDesc* cajeta_param_desc(
        void* rtti, int32_t isCtor, int32_t memberIdx, int32_t paramIdx) {
    if (!rtti) return NULL;
    CajetaRtti* r = (CajetaRtti*) rtti;
    const CajetaMethodDesc* tbl = isCtor ? r->constructors : r->methods;
    int32_t cnt = isCtor ? r->constructorCount : r->methodCount;
    if (!tbl || memberIdx < 0 || memberIdx >= cnt) return NULL;
    const CajetaMethodDesc* m = &tbl[memberIdx];
    if (!m->parameters || paramIdx < 0 || paramIdx >= m->parameterCount) return NULL;
    return &m->parameters[paramIdx];
}
// Copies `s` into the cajeta byte array `out`, whose capacity is its header and whose
// payload starts at +8; the copy truncates to that capacity.
static void cajeta_copy_into(const char* s, void* out) {
    if (!out) return;
    if (!s) s = "";
    int64_t cap = *((int64_t*) out);
    int64_t len = (int64_t) strlen(s);
    if (len > cap) len = cap;
    if (len > 0) memcpy((char*) out + 8, s, (size_t) len);
}
int32_t __cajeta_rtti_param_name_len(void* rtti, int32_t isCtor, int32_t mIdx, int32_t pIdx) {
    const CajetaParamDesc* p = cajeta_param_desc(rtti, isCtor, mIdx, pIdx);
    return (p && p->name) ? (int32_t) strlen(p->name) : 0;
}
void __cajeta_rtti_param_name_into(void* rtti, int32_t isCtor, int32_t mIdx, int32_t pIdx, void* out) {
    const CajetaParamDesc* p = cajeta_param_desc(rtti, isCtor, mIdx, pIdx);
    cajeta_copy_into(p ? p->name : "", out);
}
int32_t __cajeta_rtti_param_type_len(void* rtti, int32_t isCtor, int32_t mIdx, int32_t pIdx) {
    const CajetaParamDesc* p = cajeta_param_desc(rtti, isCtor, mIdx, pIdx);
    return (p && p->type) ? (int32_t) strlen(p->type) : 0;
}
void __cajeta_rtti_param_type_into(void* rtti, int32_t isCtor, int32_t mIdx, int32_t pIdx, void* out) {
    const CajetaParamDesc* p = cajeta_param_desc(rtti, isCtor, mIdx, pIdx);
    cajeta_copy_into(p ? p->type : "", out);
}

// An owner's annotation descriptors, with the count written to *outCount. `ownerKind`:
// 0 class, 1 field, 2 method, 3 ctor, 4/5 parameter `subIndex` of method/ctor `ownerIndex`.
static const CajetaAnnotationDesc* cajeta_annotation_list(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t* outCount) {
    *outCount = 0;
    if (!rtti) return NULL;
    CajetaRtti* r = (CajetaRtti*) rtti;
    switch (ownerKind) {
        case 0:
            *outCount = r->classAnnotationCount;
            return r->classAnnotations;
        case 1:
            if (ownerIndex < 0 || ownerIndex >= r->propertyCount || !r->properties)
                return NULL;
            *outCount = r->properties[ownerIndex].annotationCount;
            return r->properties[ownerIndex].annotations;
        case 2:
            if (ownerIndex < 0 || ownerIndex >= r->methodCount || !r->methods)
                return NULL;
            *outCount = r->methods[ownerIndex].annotationCount;
            return r->methods[ownerIndex].annotations;
        case 3:
            if (ownerIndex < 0 || ownerIndex >= r->constructorCount || !r->constructors)
                return NULL;
            *outCount = r->constructors[ownerIndex].annotationCount;
            return r->constructors[ownerIndex].annotations;
        case 4:
        case 5: {
            const CajetaParamDesc* p =
                cajeta_param_desc(rtti, ownerKind == 5, ownerIndex, subIndex);
            if (!p) return NULL;
            *outCount = p->annotationCount;
            return p->annotations;
        }
        default:
            return NULL;
    }
}
int32_t __cajeta_rtti_annotation_count(void* rtti, int32_t ownerKind,
                                       int32_t ownerIndex, int32_t subIndex) {
    int32_t n = 0;
    cajeta_annotation_list(rtti, ownerKind, ownerIndex, subIndex, &n);
    return n;
}
// One annotation descriptor by its owner locator plus `annIdx`; NULL when out of range.
static const CajetaAnnotationDesc* cajeta_annotation_desc(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx) {
    int32_t n = 0;
    const CajetaAnnotationDesc* list =
        cajeta_annotation_list(rtti, ownerKind, ownerIndex, subIndex, &n);
    if (!list || annIdx < 0 || annIdx >= n) return NULL;
    return &list[annIdx];
}
static const char* cajeta_annotation_name(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx) {
    const CajetaAnnotationDesc* d =
        cajeta_annotation_desc(rtti, ownerKind, ownerIndex, subIndex, annIdx);
    return (d && d->name) ? d->name : "";
}
int32_t __cajeta_rtti_annotation_name_len(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx) {
    return (int32_t) strlen(
        cajeta_annotation_name(rtti, ownerKind, ownerIndex, subIndex, annIdx));
}
void __cajeta_rtti_annotation_name_into(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, void* out) {
    cajeta_copy_into(
        cajeta_annotation_name(rtti, ownerKind, ownerIndex, subIndex, annIdx), out);
}

// Annotation ARGUMENT values, by owner locator plus `annIdx` and `argIdx`; out of range
// answers a sentinel (0, "" or kind -1).
static const CajetaAnnotationArgDesc* cajeta_annotation_arg(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, int32_t argIdx) {
    const CajetaAnnotationDesc* d =
        cajeta_annotation_desc(rtti, ownerKind, ownerIndex, subIndex, annIdx);
    if (!d || !d->args || argIdx < 0 || argIdx >= d->argCount) return NULL;
    return &d->args[argIdx];
}
int32_t __cajeta_rtti_annotation_arg_count(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx) {
    const CajetaAnnotationDesc* d =
        cajeta_annotation_desc(rtti, ownerKind, ownerIndex, subIndex, annIdx);
    return d ? (int32_t) d->argCount : 0;
}
int32_t __cajeta_rtti_annotation_arg_kind(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, int32_t argIdx) {
    const CajetaAnnotationArgDesc* a =
        cajeta_annotation_arg(rtti, ownerKind, ownerIndex, subIndex, annIdx, argIdx);
    return a ? a->kind : -1;
}
int64_t __cajeta_rtti_annotation_arg_int(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, int32_t argIdx) {
    const CajetaAnnotationArgDesc* a =
        cajeta_annotation_arg(rtti, ownerKind, ownerIndex, subIndex, annIdx, argIdx);
    return a ? a->i64Val : 0;
}
int32_t __cajeta_rtti_annotation_arg_bool(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, int32_t argIdx) {
    const CajetaAnnotationArgDesc* a =
        cajeta_annotation_arg(rtti, ownerKind, ownerIndex, subIndex, annIdx, argIdx);
    return (a && a->boolVal) ? 1 : 0;
}
static const char* cajeta_annotation_arg_name(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, int32_t argIdx) {
    const CajetaAnnotationArgDesc* a =
        cajeta_annotation_arg(rtti, ownerKind, ownerIndex, subIndex, annIdx, argIdx);
    return (a && a->name) ? a->name : "";
}
int32_t __cajeta_rtti_annotation_arg_name_len(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, int32_t argIdx) {
    return (int32_t) strlen(
        cajeta_annotation_arg_name(rtti, ownerKind, ownerIndex, subIndex, annIdx, argIdx));
}
void __cajeta_rtti_annotation_arg_name_into(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, int32_t argIdx, void* out) {
    cajeta_copy_into(
        cajeta_annotation_arg_name(rtti, ownerKind, ownerIndex, subIndex, annIdx, argIdx), out);
}
static const char* cajeta_annotation_arg_str(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, int32_t argIdx) {
    const CajetaAnnotationArgDesc* a =
        cajeta_annotation_arg(rtti, ownerKind, ownerIndex, subIndex, annIdx, argIdx);
    return (a && a->strVal) ? a->strVal : "";
}
int32_t __cajeta_rtti_annotation_arg_str_len(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, int32_t argIdx) {
    return (int32_t) strlen(
        cajeta_annotation_arg_str(rtti, ownerKind, ownerIndex, subIndex, annIdx, argIdx));
}
void __cajeta_rtti_annotation_arg_str_into(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, int32_t argIdx, void* out) {
    cajeta_copy_into(
        cajeta_annotation_arg_str(rtti, ownerKind, ownerIndex, subIndex, annIdx, argIdx), out);
}

// List-valued arguments: arg->listData is int64[], char*[] or int8[] by the arg's kind.
int32_t __cajeta_rtti_annotation_arg_list_count(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, int32_t argIdx) {
    const CajetaAnnotationArgDesc* a =
        cajeta_annotation_arg(rtti, ownerKind, ownerIndex, subIndex, annIdx, argIdx);
    return a ? a->listCount : 0;
}
int64_t __cajeta_rtti_annotation_arg_list_int(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, int32_t argIdx, int32_t elemIdx) {
    const CajetaAnnotationArgDesc* a =
        cajeta_annotation_arg(rtti, ownerKind, ownerIndex, subIndex, annIdx, argIdx);
    if (!a || a->kind != CAJETA_AK_INT64LIST || !a->listData
            || elemIdx < 0 || elemIdx >= a->listCount) return 0;
    return ((const int64_t*) a->listData)[elemIdx];
}
int32_t __cajeta_rtti_annotation_arg_list_bool(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, int32_t argIdx, int32_t elemIdx) {
    const CajetaAnnotationArgDesc* a =
        cajeta_annotation_arg(rtti, ownerKind, ownerIndex, subIndex, annIdx, argIdx);
    if (!a || a->kind != CAJETA_AK_BOOLLIST || !a->listData
            || elemIdx < 0 || elemIdx >= a->listCount) return 0;
    return ((const int8_t*) a->listData)[elemIdx] ? 1 : 0;
}
static const char* cajeta_annotation_arg_list_str(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, int32_t argIdx, int32_t elemIdx) {
    const CajetaAnnotationArgDesc* a =
        cajeta_annotation_arg(rtti, ownerKind, ownerIndex, subIndex, annIdx, argIdx);
    if (!a || a->kind != CAJETA_AK_STRINGLIST || !a->listData
            || elemIdx < 0 || elemIdx >= a->listCount) return "";
    const char* s = ((const char* const*) a->listData)[elemIdx];
    return s ? s : "";
}
int32_t __cajeta_rtti_annotation_arg_list_str_len(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, int32_t argIdx, int32_t elemIdx) {
    return (int32_t) strlen(cajeta_annotation_arg_list_str(
        rtti, ownerKind, ownerIndex, subIndex, annIdx, argIdx, elemIdx));
}
void __cajeta_rtti_annotation_arg_list_str_into(void* rtti, int32_t ownerKind,
        int32_t ownerIndex, int32_t subIndex, int32_t annIdx, int32_t argIdx, int32_t elemIdx, void* out) {
    cajeta_copy_into(cajeta_annotation_arg_list_str(
        rtti, ownerKind, ownerIndex, subIndex, annIdx, argIdx, elemIdx), out);
}

// Template reflection: the declared parameters and the concrete arguments an
// instantiation was materialized with, each read by index off the #Rtti.
int32_t __cajeta_rtti_template_param_count(void* rtti) {
    return rtti ? (int32_t) ((CajetaRtti*) rtti)->templateParamCount : 0;
}
static const CajetaTemplateParamDesc* cajeta_template_param(void* rtti, int32_t idx) {
    if (!rtti) return NULL;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->templateParamCount || !r->templateParams) return NULL;
    return &r->templateParams[idx];
}
static const char* cajeta_template_param_name(void* rtti, int32_t idx) {
    const CajetaTemplateParamDesc* p = cajeta_template_param(rtti, idx);
    return (p && p->name) ? p->name : "";
}
int32_t __cajeta_rtti_template_param_name_len(void* rtti, int32_t idx) {
    return (int32_t) strlen(cajeta_template_param_name(rtti, idx));
}
void __cajeta_rtti_template_param_name_into(void* rtti, int32_t idx, void* out) {
    cajeta_copy_into(cajeta_template_param_name(rtti, idx), out);
}
int32_t __cajeta_rtti_template_param_is_nontype(void* rtti, int32_t idx) {
    const CajetaTemplateParamDesc* p = cajeta_template_param(rtti, idx);
    return (p && p->isNonType) ? 1 : 0;
}
static const char* cajeta_template_param_nontype(void* rtti, int32_t idx) {
    const CajetaTemplateParamDesc* p = cajeta_template_param(rtti, idx);
    return (p && p->nonTypePrimitive) ? p->nonTypePrimitive : "";
}
int32_t __cajeta_rtti_template_param_nontype_len(void* rtti, int32_t idx) {
    return (int32_t) strlen(cajeta_template_param_nontype(rtti, idx));
}
void __cajeta_rtti_template_param_nontype_into(void* rtti, int32_t idx, void* out) {
    cajeta_copy_into(cajeta_template_param_nontype(rtti, idx), out);
}
int32_t __cajeta_rtti_template_param_bound_count(void* rtti, int32_t idx) {
    const CajetaTemplateParamDesc* p = cajeta_template_param(rtti, idx);
    return p ? (int32_t) p->boundCount : 0;
}
static const char* cajeta_template_param_bound(void* rtti, int32_t idx, int32_t boundIdx) {
    const CajetaTemplateParamDesc* p = cajeta_template_param(rtti, idx);
    if (!p || !p->bounds || boundIdx < 0 || boundIdx >= p->boundCount) return "";
    const char* b = p->bounds[boundIdx];
    return b ? b : "";
}
int32_t __cajeta_rtti_template_param_bound_len(void* rtti, int32_t idx, int32_t boundIdx) {
    return (int32_t) strlen(cajeta_template_param_bound(rtti, idx, boundIdx));
}
void __cajeta_rtti_template_param_bound_into(void* rtti, int32_t idx, int32_t boundIdx, void* out) {
    cajeta_copy_into(cajeta_template_param_bound(rtti, idx, boundIdx), out);
}

int32_t __cajeta_rtti_template_arg_count(void* rtti) {
    return rtti ? (int32_t) ((CajetaRtti*) rtti)->templateArgCount : 0;
}
static const char* cajeta_template_arg_name(void* rtti, int32_t idx) {
    if (!rtti) return "";
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->templateArgCount || !r->templateArgs) return "";
    const char* n = r->templateArgs[idx];
    return n ? n : "";
}
int32_t __cajeta_rtti_template_arg_name_len(void* rtti, int32_t idx) {
    return (int32_t) strlen(cajeta_template_arg_name(rtti, idx));
}
void __cajeta_rtti_template_arg_name_into(void* rtti, int32_t idx, void* out) {
    cajeta_copy_into(cajeta_template_arg_name(rtti, idx), out);
}

// Classifies a return type so invokeBoxed can pick a wrapper without re-parsing the type
// string: OTHER is a wrapper-less primitive, a non-primitive is REFERENCE. Mirrored by
// REFLECT_KIND_* in Method.cajeta.
#define CAJETA_RK_VOID       0
#define CAJETA_RK_BOOLEAN    1
#define CAJETA_RK_INT32      2
#define CAJETA_RK_INT64      3
#define CAJETA_RK_FLOAT32    4
#define CAJETA_RK_FLOAT64    5
#define CAJETA_RK_REFERENCE  6
#define CAJETA_RK_OTHER      7
// W2 widths: boxed through the 64-bit machinery with truncation.
#define CAJETA_RK_INT8       8
#define CAJETA_RK_INT16      9
#define CAJETA_RK_UINT8      10
#define CAJETA_RK_UINT16     11
#define CAJETA_RK_UINT32     12
#define CAJETA_RK_UINT64     13
#define CAJETA_RK_CHAR       14
static int32_t cajeta_return_kind(const char* t) {
    if (!t) return CAJETA_RK_REFERENCE;
    if (!strcmp(t, "void"))    return CAJETA_RK_VOID;
    if (!strcmp(t, "boolean")) return CAJETA_RK_BOOLEAN;
    if (!strcmp(t, "int32"))   return CAJETA_RK_INT32;
    if (!strcmp(t, "int64"))   return CAJETA_RK_INT64;
    if (!strcmp(t, "float32")) return CAJETA_RK_FLOAT32;
    if (!strcmp(t, "float64")) return CAJETA_RK_FLOAT64;
    if (!strcmp(t, "int8"))    return CAJETA_RK_INT8;
    if (!strcmp(t, "int16"))   return CAJETA_RK_INT16;
    if (!strcmp(t, "uint8") || !strcmp(t, "uchar")) return CAJETA_RK_UINT8;
    if (!strcmp(t, "uint16"))  return CAJETA_RK_UINT16;
    if (!strcmp(t, "uint32"))  return CAJETA_RK_UINT32;
    if (!strcmp(t, "uint64"))  return CAJETA_RK_UINT64;
    if (!strcmp(t, "char"))    return CAJETA_RK_CHAR;
    // Primitives with no wrapper: widening would lie about the boxed type's identity.
    if (!strcmp(t, "int128")  || !strcmp(t, "uint128")  || !strcmp(t, "float16") ||
        !strcmp(t, "bfloat16")|| !strcmp(t, "float128") || !strcmp(t, "pointer") ||
        !strcmp(t, "float4e2m1")     || !strcmp(t, "float6e2m3")     ||
        !strcmp(t, "float6e3m2")     || !strcmp(t, "float8e4m3")     ||
        !strcmp(t, "float8e5m2")     || !strcmp(t, "float8e4m3fnuz") ||
        !strcmp(t, "float8e5m2fnuz")) return CAJETA_RK_OTHER;
    return CAJETA_RK_REFERENCE;
}
int32_t __cajeta_rtti_method_return_kind(void* rtti, int32_t idx) {
    if (!rtti) return CAJETA_RK_REFERENCE;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->methodCount || !r->methods) return CAJETA_RK_REFERENCE;
    return cajeta_return_kind(r->methods[idx].returnType);
}
// The same classification for a field, so Field.getBoxed refuses a reference field.
int32_t __cajeta_rtti_field_kind(void* rtti, int32_t idx) {
    if (!rtti) return CAJETA_RK_REFERENCE;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->propertyCount || !r->properties) return CAJETA_RK_REFERENCE;
    return cajeta_return_kind(r->properties[idx].type);
}

// REFL-2C constructor introspection + reflective construction.
int32_t __cajeta_rtti_constructor_count(void* rtti) {
    return rtti ? (int32_t) ((CajetaRtti*) rtti)->constructorCount : 0;
}
int32_t __cajeta_rtti_constructor_param_count(void* rtti, int32_t idx) {
    if (!rtti) return -1;
    CajetaRtti* r = (CajetaRtti*) rtti;
    if (idx < 0 || idx >= r->constructorCount || !r->constructors) return -1;
    return r->constructors[idx].parameterCount;
}
// Constructs through the class's no-arg newInstance adapter; the object is the caller's.
void* __cajeta_class_new0(void* rtti, int32_t ctorIdx) {
    if (!rtti) return NULL;
    void* (*adapter)(int32_t, void*) =
        (void* (*)(int32_t, void*)) ((CajetaRtti*) rtti)->newInstanceAdapter;
    if (!adapter) return NULL;
    if (cajeta_reflect_trace())
        fprintf(stderr, "[refl] new0 type=%s ctorIdx=%d adapter=%p\n",
                ((CajetaRtti*) rtti)->typeName, ctorIdx, (void*) adapter);
    return adapter(ctorIdx, NULL);
}
// Reflective construction WITH arguments: `argArray` is a cajeta int64[] of the form
// { count, elems } or NULL, and the adapter is handed the element region.
void* __cajeta_class_new(void* rtti, int32_t ctorIdx, void* argArray) {
    if (!rtti) return NULL;
    void* (*adapter)(int32_t, void*) =
        (void* (*)(int32_t, void*)) ((CajetaRtti*) rtti)->newInstanceAdapter;
    if (!adapter) return NULL;
    if (cajeta_reflect_trace())
        fprintf(stderr, "[refl] newN type=%s ctorIdx=%d adapter=%p\n",
                ((CajetaRtti*) rtti)->typeName, ctorIdx, (void*) adapter);
    void* args = argArray ? (void*) ((char*) argArray + 8) : NULL;
    return adapter(ctorIdx, args);
}

// UnrecoverableException's vtable, published by a codegen-emitted global ctor: a ctor plus
// a plain call binds identically on ELF/MachO/COFF and under both JIT and AOT.
static void* g_unrecoverable_vtable = NULL;

void __cajeta_set_unrecoverable_vtable(void* vtable) {
    g_unrecoverable_vtable = vtable;
}

// 1 when a Throwable's vtable parent chain reaches the UnrecoverableException vtable.
int32_t __cajeta_is_unrecoverable(void* throwable) {
    if (!throwable) return 0;
    // A legacy `throw 42` arrives IntToPtr'd; reading a vtable through it would SIGSEGV.
    if ((uintptr_t) throwable < 4096) return 0;
    void* vtable = *(void**) throwable;   // instance slot 0 = vtable ptr
    if (!g_unrecoverable_vtable) return 0;
    while (vtable) {
        if (vtable == g_unrecoverable_vtable) return 1;
        vtable = *(void**) ((char*) vtable + CAJETA_VTABLE_PARENT_OFFSET);
    }
    return 0;
}

// 1 iff `catch_vtable` is in the thrown object's vtable parent chain, so the thrown class
// is the catch class or a descendant. A catch-all's null vtable is handled in codegen.
int32_t __cajeta_exc_matches(void* throwable, void* catch_vtable) {
    if (!throwable || !catch_vtable) return 0;
    // The same zero-page guard as the unrecoverable walk.
    if ((uintptr_t) throwable < 4096) return 0;
    void* vtable = *(void**) throwable;   // instance slot 0 = vtable ptr
    // A capped, address-checked walk: a malformed chain answers no-match, never faults.
    for (int depth = 0; depth < 256; ++depth) {
        if ((uintptr_t) vtable < 4096) break;
        if (vtable == catch_vtable) return 1;
        vtable = *(void**) ((char*) vtable + CAJETA_VTABLE_PARENT_OFFSET);
    }
    return 0;
}

// Forward decl — defined alongside __cajeta_throw further down.
static void __cajeta_emit_uncaught(void* value, int is_unrec);

// The fiber trampoline's catch: an Unrecoverable aborts rather than hide behind await.
void __cajeta_fiber_handle_throw(void* thrown) {
    if (__cajeta_is_unrecoverable(thrown)) {
        __cajeta_emit_uncaught(thrown, /*is_unrec=*/1);
        abort();
    }
}

struct cajeta_exception_frame {
    jmp_buf buf;
    struct cajeta_exception_frame* prev;
    // A bare pointer here, a Throwable* to codegen; the legacy int-throw idiom
    // round-trips through IntToPtr and PtrToInt.
    void* thrown_value;
    // Drop-chain watermark snapshotted at try-entry. On throw, the runtime
    // unwinds drops between the current top and this watermark before longjmp.
    struct cajeta_drop_entry* drop_watermark;
    // Line-info shadow depth at try-entry; unwound frames run no __cajeta_line_leave.
    int32_t shadow_watermark;
    // Instrumentation probe depth, restored on catch for the same reason.
    int32_t instr_watermark;
    // Debug frame-chain head at try-entry; unwound frames run no dbg_frame_leave either.
    struct cajeta_dbg_frame* dbg_watermark;
};

// The per-try-frame blob size the IR side allocates, exposed so the JIT can check it.
size_t __cajeta_exc_frame_size(void) {
    return sizeof(struct cajeta_exception_frame);
}

// Exception chain head for the main thread; a fiber uses the slot in its own struct.
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
    f->drop_watermark = *dropTop;
    f->shadow_watermark = __cajeta_shadow_get_top();
    f->instr_watermark = __cajeta_prof_instr_depth();
    f->dbg_watermark = *__cajeta_dbg_top_ptr();
    *top = f;
}

void __cajeta_exc_pop(void) {
    struct cajeta_exception_frame** top = __cajeta_exc_top_ptr();
    if (*top) {
        *top = (*top)->prev;
    }
}

// --- R5/Error-model #203: stack-trace capture ---------------------------
