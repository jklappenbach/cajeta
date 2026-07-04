// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c
// === (single-TU build; not a standalone compilation unit).
// ---- @Inject runtime override registry (test-only DI substitution) ---------
//
// Lets a test bind a substitute instance for a type so that a `@Inject` site
// whose field type matches resolves to the substitute instead of the
// statically-wired provider. Keyed by the type's `reflect.Class` object pointer
// — what `T.class` lowers to (the named, linker-unified `<type>#ClassObject`
// global) — so matching is pointer identity, no string compare.
//
// The compiler only emits the lookup in TEST builds (activeProfile == "test"),
// so production injection paths carry zero overhead and don't link this at all
// unless used. Entries hold BORROWED pointers: the test owns the substitute for
// its lifetime; clear() forgets entries, it never frees the instances.
//
// v1 scope: singleton-mode, class-typed `@Inject` fields (mock by subclassing
// and overriding virtuals). Interface-typed fields need a fat-pointer-aware
// path and are not yet overridable.
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

// REFL-4 typed FP return paths. The per-class invoke adapter already stores a
// float/double result into the 8-byte `ret` buffer (emitReflectInvokeBody
// marshals floating-point returns); these variants read that buffer in the FP
// register so the value crosses the native boundary as a real float/double
// instead of as raw bits widened to int64. `argArray` is the same int64[]
// element-region convention as the scalar path. Resolve the adapter once via a
// shared helper to avoid duplicating the vtable->classObject->rtti walk.
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
    // 8-byte buffer; the adapter stores a 4-byte float into its low bytes.
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
// REFL-4: invoke a method whose return type is a reference (object/pointer).
// The per-class adapter stores the returned pointer to the ret buffer (the
// marshaller accepts isPointerTy returns); we read it back whole. Ownership
// transfers per the invoked method's signature — a method returning `heap T`
// hands the caller an owned reference (Method.invokeObject is typed #Object so
// the result is drop-tracked); a method returning a borrow would be unsafe to
// reflect this way (documented on Method.invokeObject).
void* __cajeta_object_invoke_obj(void* obj, int32_t idx, void* argArray) {
    void (*adapter)(void*, int32_t, void*, void*) =
        (void (*)(void*, int32_t, void*, void*)) cajeta_resolve_invoke_adapter(obj);
    if (!adapter) return NULL;
    void* args = argArray ? (void*) ((char*) argArray + 8) : NULL;
    void* ret = NULL;
    adapter(obj, idx, args, &ret);
    return ret;
}

// REFL-4.4 (Strategy 6): fiber-stack argument buffers. For a small, statically
// known argument count the caller hands the raw args as discrete int64
// parameters instead of building a heap int64[]. Each native assembles the
// adapter's 8-byte-strided arg buffer (`buf`) on its own C stack frame — which
// IS the calling fiber's stack — so there is no heap allocation and no count
// header to skip past. Result is widened to int64; the cajeta layer narrows to
// int32 where wanted, exactly as the int64[]-path variants do. (FP-return and
// reference-return stack-arg siblings are a mechanical extension of this same
// `buf` pattern, reading the ret buffer as float/double/pointer instead.)
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

// REFL-4 parameter introspection. `isCtor` selects the constructor table vs
// the method table; memberIdx is the method/constructor index; paramIdx is the
// USER parameter index (the implicit `this` is excluded from the table).
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

// REFL-6a annotation NAME reflection. Every annotatable owner stores its
// annotation type names as a (count, const char**) pair in the RTTI; this one
// resolver addresses any of them so the cajeta side needs a single native
// family. `ownerKind` selects the owner:
//   0 = class, 1 = field[ownerIndex], 2 = method[ownerIndex],
//   3 = constructor[ownerIndex],
//   4 = parameter subIndex of method[ownerIndex],
//   5 = parameter subIndex of constructor[ownerIndex].
// `ownerIndex` is the field/method/ctor index (ignored for the class); for the
// parameter kinds it is the owning member's index and `subIndex` is the
// user-visible parameter position. Returns the name array and writes its length
// to *outCount; NULL (count 0) for an out-of-range owner. Each descriptor
// carries the annotation's canonical name (REFL-6a) plus its captured argument
// values (REFL-6b).
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
// Resolve one annotation descriptor by its locator (rtti + ownerKind/
// ownerIndex/subIndex + annIdx). NULL when out of range.
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

// REFL-6b annotation ARGUMENT VALUE reflection. Indexed by the same owner
// locator as the name natives, plus `annIdx` (which annotation) and `argIdx`
// (which argument within it). All return a sentinel (0 / "" / kind -1) for an
// out-of-range locator so the cajeta side reads uniformly.
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

// REFL-6b list-valued arguments (`@SuppressLint({"a","b"})`, `@Sizes({1,2})`,
// `@Flags({true,false})`). Element data lives in arg->listData, shaped by the
// arg's kind: int64[] (Int64List), char*[] (StringList), int8[] (BoolList).
// elemIdx selects within the list; out-of-range reads return a sentinel.
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

// REFL-7 template reflection. A template instantiation (Box<int32>) carries its
// declared template parameters (the `<T>`) and the concrete template arguments
// it was materialized with (int32). Both are read by index off the #Rtti.
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

// REFL-4.1 (boxing, plan W5): classify a method's return type into a compact
// kind so Method.invokeBoxed can pick the right wrapper / invoke path without
// re-parsing the type string in cajeta. The kinds the W1 wrapper family boxes
// exactly map to their own value; OTHER is a primitive with no W1 wrapper yet
// (int8/16/128, unsigned, char, ML floats, raw pointer) — invokeBoxed throws
// for those until W2-W4 land. A non-primitive return is REFERENCE (boxed via
// the existing invokeObject path). Keep these constants in sync with the
// REFLECT_KIND_* mirror in Method.cajeta.
#define CAJETA_RK_VOID       0
#define CAJETA_RK_BOOLEAN    1
#define CAJETA_RK_INT32      2
#define CAJETA_RK_INT64      3
#define CAJETA_RK_FLOAT32    4
#define CAJETA_RK_FLOAT64    5
#define CAJETA_RK_REFERENCE  6
#define CAJETA_RK_OTHER      7
// W2 widths (box through the 64-bit invoke/field machinery + truncation; the
// 8/16-bit field reads use dedicated width-correct natives below).
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
    // Primitives still without a wrapper — 128-bit ints (don't fit the 64-bit
    // paths) and the half/quad/ML floats + raw pointer. Not boxable yet
    // (honest, rather than widening and lying about the boxed type's identity).
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
// W5b: same classification for a field's declared type, so Field.getBoxed can
// pick the wrapper / refuse a reference field (which can't be handed back as an
// owned #Object without a borrow-return surface). Reuses the field-desc `type`
// string (CajetaFieldDesc also carries typeFlags, but the string keeps one
// shared classifier with the method path).
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
// Reflectively construct an instance via the class's newInstance adapter
// (no-arg constructor path). `rtti` is the class's #Rtti pointer (a Class
// instance's `rtti` field). Returns the new object (owned by the caller) or
// NULL if there's no adapter / the index isn't a marshallable constructor.
void* __cajeta_class_new0(void* rtti, int32_t ctorIdx) {
    if (!rtti) return NULL;
    void* (*adapter)(int32_t, void*) =
        (void* (*)(int32_t, void*)) ((CajetaRtti*) rtti)->newInstanceAdapter;
    if (!adapter) return NULL;
    return adapter(ctorIdx, NULL);
}
// REFL-4 reflective construction WITH arguments. `argArray` is a cajeta
// int64[] ({ count, elems }) or NULL; hand the adapter the element region.
void* __cajeta_class_new(void* rtti, int32_t ctorIdx, void* argArray) {
    if (!rtti) return NULL;
    void* (*adapter)(int32_t, void*) =
        (void* (*)(int32_t, void*)) ((CajetaRtti*) rtti)->newInstanceAdapter;
    if (!adapter) return NULL;
    void* args = argArray ? (void*) ((char*) argArray + 8) : NULL;
    return adapter(ctorIdx, args);
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
    // Line-info shadow-stack depth at try-entry (diagnostic-exceptions U3). A
    // throw doesn't run __cajeta_line_leave for the frames it unwinds, so on
    // catch __cajeta_throw restores __cajeta_shadow_top to this value.
    int32_t shadow_watermark;
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
    // Snapshot the shadow line-stack depth so a caught throw restores it (U3).
    f->shadow_watermark = __cajeta_shadow_top;
    *top = f;
}

void __cajeta_exc_pop(void) {
    struct cajeta_exception_frame** top = __cajeta_exc_top_ptr();
    if (*top) {
        *top = (*top)->prev;
    }
}

// --- R5/Error-model #203: stack-trace capture ---------------------------
