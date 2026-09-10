// cajeta.lang.String natives over the tagged core. Wrapper layout (cajeta_rt_core.c):
//   { vtable@0, i32 lenTag@8, i32 aux@12, ptr base@16, i32 cachedCpLength@24 }
// aux+base are the inline bytes (len <= 12) or the {window offset, root} pair.

// Take FULL ownership of the transferred buffer; both arms must consume it.
void __cajeta_string_adopt(void* s_v, void* buf, int32_t n) {
    cajeta_string_layout* s = (cajeta_string_layout*) s_v;
    s->cachedCpLength = -1;
    if (n < 0) n = 0;
    if (!buf) {
        caj_str_set_inline(s, NULL, 0);
        return;
    }
    if (n <= CAJ_STR_INLINE_CAP) {
        caj_str_set_inline(s, (const char*) buf + 8, n);
        __cajeta_free_array(buf);
        return;
    }
    caj_str_set_window(s, n, 0, buf);
}

// Byte length and byte pointer for a debugger; `used, retain`, never called by codegen.
__attribute__((used, retain))
int32_t __cajeta_string_byte_len(void* s_v) {
    if (!s_v) return 0;
    return caj_str_len((const cajeta_string_layout*) s_v);
}

__attribute__((used, retain))
const char* __cajeta_string_bytes(void* s_v) {
    if (!s_v) return "";
    const char* p = caj_str_ptr((const cajeta_string_layout*) s_v);
    return p ? p : "";
}

// Build a fresh String wrapper from raw bytes; copies, so the caller keeps `data`.
void* __cajeta_string_from_buf(const char* data, int64_t len, void* vtable) {
    cajeta_string_layout* out =
        (cajeta_string_layout*) __cajeta_alloc(sizeof(cajeta_string_layout));
    out->vtable = vtable;
    out->cachedCpLength = -1;
    if (len < 0) len = 0;
    if (len <= CAJ_STR_INLINE_CAP) {
        caj_str_set_inline(out, data, (int32_t) len);
    } else {
        void* buf = caj_str_new_root(data, (int32_t) len);
        caj_str_set_window(out, (int32_t) len, 0, buf);
    }
    return out;
}

// XXH3 over the window: representation-independent, for Utf8.hash parity.
int64_t __cajeta_string_hash(void* s_v) {
    cajeta_string_layout* s = (cajeta_string_layout*) s_v;
    int64_t __cajeta_hash_bytes(const uint8_t* data, int64_t len);
    return __cajeta_hash_bytes((const uint8_t*) caj_str_ptr(s),
                               (int64_t) caj_str_len(s));
}

int32_t __cajeta_string_equals(void* a_v, void* b_v) {
    cajeta_string_layout* a = (cajeta_string_layout*) a_v;
    cajeta_string_layout* b = (cajeta_string_layout*) b_v;
    int32_t n = caj_str_len(a);
    if (n != caj_str_len(b)) return 0;
    if (n == 0) return 1;
    return memcmp(caj_str_ptr(a), caj_str_ptr(b), (size_t) n) == 0;
}

// Raw byte at `idx`, 0 when out of range (the charAt convention, shared by byteAt).
int8_t __cajeta_string_byte_at(void* s_v, int32_t idx) {
    cajeta_string_layout* s = (cajeta_string_layout*) s_v;
    if (idx < 0 || idx >= caj_str_len(s)) return 0;
    return (int8_t) caj_str_ptr(s)[idx];
}

// First byte-index of needle's window in s's window at/after `start`, or -1.
int64_t __cajeta_string_index_of(void* s_v, void* n_v, int64_t start) {
    cajeta_string_layout* s = (cajeta_string_layout*) s_v;
    cajeta_string_layout* n = (cajeta_string_layout*) n_v;
    if (!n) return -1;
    int64_t hlen = (int64_t) caj_str_len(s);
    int64_t nlen = (int64_t) caj_str_len(n);
    if (start < 0) start = 0;
    if (nlen == 0) return start <= hlen ? start : -1;
    if (nlen > hlen - start) return -1;
    const char* h = caj_str_ptr(s);
    const char* nd = caj_str_ptr(n);
    char first = nd[0];
    char last = nd[nlen - 1];
    const char* p = h + start;
    const char* lim = h + hlen - nlen;             // last valid start
    while (p <= lim) {
        const char* c = (const char*) memchr(p, first, (size_t) (lim - p + 1));
        if (!c) return -1;
        if (c[nlen - 1] == last
                && (nlen <= 2 || memcmp(c + 1, nd + 1, (size_t) nlen - 2) == 0)) {
            return (int64_t) (c - h);
        }
        p = c + 1;
    }
    return -1;
}

int32_t __cajeta_string_starts_with(void* s_v, void* p_v) {
    cajeta_string_layout* s = (cajeta_string_layout*) s_v;
    cajeta_string_layout* p = (cajeta_string_layout*) p_v;
    if (!p) return 0;
    int32_t n = caj_str_len(p);
    if (n > caj_str_len(s)) return 0;
    if (n == 0) return 1;
    return memcmp(caj_str_ptr(s), caj_str_ptr(p), (size_t) n) == 0;
}

int32_t __cajeta_string_ends_with(void* s_v, void* p_v) {
    cajeta_string_layout* s = (cajeta_string_layout*) s_v;
    cajeta_string_layout* p = (cajeta_string_layout*) p_v;
    if (!p) return 0;
    int32_t n = caj_str_len(p);
    int32_t sl = caj_str_len(s);
    if (n > sl) return 0;
    if (n == 0) return 1;
    return memcmp(caj_str_ptr(s) + (sl - n), caj_str_ptr(p), (size_t) n) == 0;
}

// Fresh wrapper holding an ASCII case-mapped copy, reusing the receiver's vtable.
static void* caj_str_case_map(cajeta_string_layout* s, int lower) {
    int32_t n = caj_str_len(s);
    cajeta_string_layout* out =
        (cajeta_string_layout*) __cajeta_alloc(sizeof(cajeta_string_layout));
    out->vtable = s->vtable;
    out->cachedCpLength = -1;
    char* dst;
    char inl[CAJ_STR_INLINE_CAP];
    void* buf = NULL;
    if (n <= CAJ_STR_INLINE_CAP) {
        dst = inl;
    } else {
        buf = __cajeta_new_array_header(8, 1, (uint64_t) n + 1);
        *((int64_t*) buf) = n;
        ((char*) buf)[8 + n] = 0;
        dst = (char*) buf + 8;
    }
    const char* src = caj_str_ptr(s);
    for (int32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) src[i];
        if (lower) {
            dst[i] = (char) ((c >= 'A' && c <= 'Z') ? c + 32 : c);
        } else {
            dst[i] = (char) ((c >= 'a' && c <= 'z') ? c - 32 : c);
        }
    }
    if (n <= CAJ_STR_INLINE_CAP) {
        caj_str_set_inline(out, inl, n);
    } else {
        caj_str_set_window(out, n, 0, buf);
    }
    return out;
}

void* __cajeta_string_upper(void* s_v) {
    return caj_str_case_map((cajeta_string_layout*) s_v, 0);
}

void* __cajeta_string_lower(void* s_v) {
    return caj_str_case_map((cajeta_string_layout*) s_v, 1);
}

// Replace every non-overlapping `from` with `repl`; NULL means "no change".
void* __cajeta_string_replace(void* s_v, void* f_v, void* r_v) {
    cajeta_string_layout* s = (cajeta_string_layout*) s_v;
    cajeta_string_layout* f = (cajeta_string_layout*) f_v;
    cajeta_string_layout* r = (cajeta_string_layout*) r_v;
    if (!f || !r) return NULL;
    int64_t flen = (int64_t) caj_str_len(f);
    if (flen == 0) return NULL;
    int64_t hlen = (int64_t) caj_str_len(s);
    int64_t rlen = (int64_t) caj_str_len(r);
    const char* h = caj_str_ptr(s);
    const char* fd = caj_str_ptr(f);
    const char* rd = caj_str_ptr(r);
    int64_t count = 0;
    for (int64_t i = 0; i + flen <= hlen; ) {
        if (h[i] == fd[0] && memcmp(h + i, fd, (size_t) flen) == 0) {
            count++;
            i += flen;
        } else {
            i++;
        }
    }
    if (count == 0) return NULL;
    int64_t outLen = hlen + count * (rlen - flen);
    cajeta_string_layout* out =
        (cajeta_string_layout*) __cajeta_alloc(sizeof(cajeta_string_layout));
    out->vtable = s->vtable;
    out->cachedCpLength = -1;
    char* dst;
    char inl[CAJ_STR_INLINE_CAP];
    void* buf = NULL;
    if (outLen <= CAJ_STR_INLINE_CAP) {
        dst = inl;
    } else {
        buf = __cajeta_new_array_header(8, 1, (uint64_t) outLen + 1);
        *((int64_t*) buf) = outLen;
        ((char*) buf)[8 + outLen] = 0;
        dst = (char*) buf + 8;
    }
    int64_t w = 0;
    for (int64_t i = 0; i < hlen; ) {
        if (i + flen <= hlen && h[i] == fd[0]
                && memcmp(h + i, fd, (size_t) flen) == 0) {
            if (rlen > 0) memcpy(dst + w, rd, (size_t) rlen);
            w += rlen;
            i += flen;
        } else {
            dst[w++] = h[i++];
        }
    }
    if (outLen <= CAJ_STR_INLINE_CAP) {
        caj_str_set_inline(out, inl, (int32_t) outLen);
    } else {
        caj_str_set_window(out, (int32_t) outLen, 0, buf);
    }
    return out;
}

// Copy the window into `dst`'s data at byte offset `dstOff` and return the byte
// count; one call per bulk consume, and the caller guarantees capacity.
int32_t __cajeta_string_copy_to(void* s_v, void* dstArr, int32_t dstOff) {
    cajeta_string_layout* s = (cajeta_string_layout*) s_v;
    int32_t n = caj_str_len(s);
    if (n > 0 && dstArr) {
        memcpy((char*) dstArr + 8 + dstOff, caj_str_ptr(s), (size_t) n);
    }
    return n;
}

// The effective window as a fresh caller-owned int8[].
void* __cajeta_string_to_bytes(void* s_v) {
    cajeta_string_layout* s = (cajeta_string_layout*) s_v;
    int32_t n = caj_str_len(s);
    void* buf = __cajeta_new_array_header(8, 1, (uint64_t) (n > 0 ? n : 0));
    *((int64_t*) buf) = n;
    if (n > 0) memcpy((char*) buf + 8, caj_str_ptr(s), (size_t) n);
    return buf;
}

// Write the window to `fd`, returning the byte count for the writer's pos.
int64_t __cajeta_file_write(int32_t fd, const void* data, int64_t len);
int32_t __cajeta_file_write_string(int32_t fd, void* s_v) {
    cajeta_string_layout* s = (cajeta_string_layout*) s_v;
    int32_t n = caj_str_len(s);
    if (n > 0) __cajeta_file_write(fd, caj_str_ptr(s), n);
    return n;
}

// Quote-and-escape a String object's window into a fresh malloc'd C string.
char* __cajeta_json_quote_buf(const char* data, int64_t n);
char* __cajeta_json_quote_string(void* s_v) {
    if (!s_v) return __cajeta_json_quote_buf(NULL, 0);
    cajeta_string_layout* s = (cajeta_string_layout*) s_v;
    return __cajeta_json_quote_buf(caj_str_ptr(s), (int64_t) caj_str_len(s));
}
