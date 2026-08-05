// cajeta.lang String normalization wrappers over the UCD core
// (stdlib-completion U7; spec §7). Pure algorithms + tables live in
// cajeta_rt_ucd_core.c (compiled standalone by the conformance suite);
// this layer only adapts the tagged String layout and follows
// __cajeta_string_replace's convention: NULL result = "no change", so the
// .cajeta side hands back `this` — §7.4's no-copy fast path.
//
// Included from cajeta_runtime.c after cajeta_rt_string.c (uses
// cajeta_string_layout + caj_str_* helpers + __cajeta_alloc).

int32_t __cajeta_ucd_utf8_valid_buf(const uint8_t* p, int64_t n);
int32_t __cajeta_ucd_is_normalized_buf(const uint8_t* p, int64_t n, int32_t form);
int64_t __cajeta_ucd_normalize_buf(const uint8_t* p, int64_t n, int32_t form,
                                   uint8_t** out);
int32_t __cajeta_ucd_fold_cp(int32_t cp, uint32_t* out);
int32_t __cajeta_ucd_decode_cp(const uint8_t* p, int64_t i, int64_t n, uint32_t* out);
int32_t __cajeta_ucd_encode_cp(uint32_t cp, uint8_t* out);
int32_t __cajeta_ucd_default_ignorable(int32_t cp);

int32_t __cajeta_ucd_utf8_valid(void* s_v) {
    const cajeta_string_layout* s = (const cajeta_string_layout*) s_v;
    return __cajeta_ucd_utf8_valid_buf((const uint8_t*) caj_str_ptr(s),
                                       (int64_t) caj_str_len(s));
}

// EXACT membership: quick-check "yes" answers immediately; a No/Maybe
// falls back to transform-and-compare, so the public boolean never lies
// on a QC-Maybe string that happens to be normalized already.
int32_t __cajeta_ucd_is_normalized(void* s_v, int32_t form) {
    const cajeta_string_layout* s = (const cajeta_string_layout*) s_v;
    const uint8_t* p = (const uint8_t*) caj_str_ptr(s);
    int64_t n = caj_str_len(s);
    if (__cajeta_ucd_is_normalized_buf(p, n, form)) return 1;
    uint8_t* out = NULL;
    int64_t w = __cajeta_ucd_normalize_buf(p, n, form, &out);
    if (w < 0) return 0;
    int32_t same = (w == n) && (n == 0 || memcmp(out, p, (size_t) n) == 0);
    free(out);
    return same;
}

// Build a fresh String wrapper around bytes (caj_str_case_map's recipe).
static void* caj_ucd_wrap(const cajeta_string_layout* like,
                          const uint8_t* bytes, int64_t n) {
    cajeta_string_layout* out =
        (cajeta_string_layout*) __cajeta_alloc(sizeof(cajeta_string_layout));
    out->vtable = like->vtable;
    out->cachedCpLength = -1;
    if (n <= CAJ_STR_INLINE_CAP) {
        caj_str_set_inline(out, (const char*) bytes, (int32_t) n);
    } else {
        void* buf = caj_str_new_root((const char*) bytes, (int32_t) n);
        caj_str_set_window(out, (int32_t) n, 0, buf);
    }
    return out;
}

// Normalize into `form` (0=NFC 1=NFD 2=NFKC 3=NFKD). NULL = already
// normalized (fast path, no copy). The .cajeta side validates UTF-8 first
// and throws EncodingException, so -1 from the core cannot happen here.
void* __cajeta_ucd_normalize(void* s_v, int32_t form) {
    const cajeta_string_layout* s = (const cajeta_string_layout*) s_v;
    const uint8_t* p = (const uint8_t*) caj_str_ptr(s);
    int64_t n = caj_str_len(s);
    if (__cajeta_ucd_is_normalized_buf(p, n, form)) return NULL;
    uint8_t* bytes = NULL;
    int64_t w = __cajeta_ucd_normalize_buf(p, n, form, &bytes);
    if (w < 0) return NULL;
    void* out = caj_ucd_wrap(s, bytes, w);
    free(bytes);
    return out;
}

// Full case folding (§7.3, distinct from toLowerCase). NULL = identity.
void* __cajeta_ucd_casefold(void* s_v) {
    const cajeta_string_layout* s = (const cajeta_string_layout*) s_v;
    const uint8_t* p = (const uint8_t*) caj_str_ptr(s);
    int64_t n = caj_str_len(s);
    uint8_t* bytes = (uint8_t*) malloc((size_t) (n * 12 + 1));
    int64_t w = 0;
    int changed = 0;
    int64_t i = 0;
    uint32_t cp;
    while (i < n) {
        int32_t len = __cajeta_ucd_decode_cp(p, i, n, &cp);
        if (len == 0) { free(bytes); return NULL; }
        uint32_t folded[3];
        int32_t cnt = __cajeta_ucd_fold_cp((int32_t) cp, folded);
        if (cnt) {
            changed = 1;
            for (int32_t k = 0; k < cnt; k++)
                w += __cajeta_ucd_encode_cp(folded[k], bytes + w);
        } else {
            memcpy(bytes + w, p + i, (size_t) len);
            w += len;
        }
        i += len;
    }
    void* out = changed ? caj_ucd_wrap(s, bytes, w) : NULL;
    free(bytes);
    return out;
}

// Strip Default_Ignorable_Code_Point characters (§7.5). NULL = nothing
// stripped.
void* __cajeta_ucd_strip_ignorable(void* s_v) {
    const cajeta_string_layout* s = (const cajeta_string_layout*) s_v;
    const uint8_t* p = (const uint8_t*) caj_str_ptr(s);
    int64_t n = caj_str_len(s);
    uint8_t* bytes = (uint8_t*) malloc((size_t) n + 1);
    int64_t w = 0;
    int changed = 0;
    int64_t i = 0;
    uint32_t cp;
    while (i < n) {
        int32_t len = __cajeta_ucd_decode_cp(p, i, n, &cp);
        if (len == 0) { free(bytes); return NULL; }
        if (__cajeta_ucd_default_ignorable((int32_t) cp)) {
            changed = 1;
        } else {
            memcpy(bytes + w, p + i, (size_t) len);
            w += len;
        }
        i += len;
    }
    void* out = changed ? caj_ucd_wrap(s, bytes, w) : NULL;
    free(bytes);
    return out;
}
