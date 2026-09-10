// Perfetto trace writer: protobuf wire primitives, then the schema mapping. A
// .pftrace is a bare sequence of length-delimited TracePacket messages, with no
// header and no footer, which is why a truncated trace stays readable.

// ── protobuf wire primitives ─────────────────────────────────────────────
#define CAJ_PB_WIRE_VARINT 0
#define CAJ_PB_WIRE_FIXED64 1
#define CAJ_PB_WIRE_BYTES  2

// Base-128 varint. Returns the bytes written (1..10); `out` must have room for 10.
int32_t __cajeta_pb_varint(uint8_t* out, uint64_t v) {
    int32_t n = 0;
    do {
        uint8_t byte = (uint8_t) (v & 0x7Fu);
        v >>= 7;
        if (v) byte |= 0x80u;
        out[n++] = byte;
    } while (v);
    return n;
}

// Inverse; sets *consumed, or -1 when the encoding runs past `max` or 10 bytes.
uint64_t __cajeta_pb_varint_read(const uint8_t* in, int32_t max, int32_t* consumed) {
    uint64_t v = 0;
    int32_t shift = 0, n = 0;
    while (n < max && n < 10) {
        uint8_t byte = in[n++];
        v |= ((uint64_t) (byte & 0x7Fu)) << shift;
        if (!(byte & 0x80u)) { if (consumed) *consumed = n; return v; }
        shift += 7;
    }
    if (consumed) *consumed = -1;
    return 0;
}

// A field tag is varint((field_number << 3) | wire_type).
int32_t __cajeta_pb_tag(uint8_t* out, uint32_t field, uint32_t wire) {
    return __cajeta_pb_varint(out, ((uint64_t) field << 3) | (uint64_t) wire);
}

// field: varint value.
int32_t __cajeta_pb_uint64(uint8_t* out, uint32_t field, uint64_t v) {
    int32_t n = __cajeta_pb_tag(out, field, CAJ_PB_WIRE_VARINT);
    return n + __cajeta_pb_varint(out + n, v);
}

// A fixed64 field: eight little-endian bytes. flow_ids as a varint draws nothing.
int32_t __cajeta_pb_fixed64(uint8_t* out, uint32_t field, uint64_t v) {
    int32_t n = __cajeta_pb_tag(out, field, CAJ_PB_WIRE_FIXED64);
    for (int32_t i = 0; i < 8; i++) out[n + i] = (uint8_t) ((v >> (i * 8)) & 0xFF);
    return n + 8;
}

// field: length-delimited payload; a string and a submessage are identical here.
int32_t __cajeta_pb_bytes(uint8_t* out, uint32_t field,
                          const uint8_t* data, int32_t len) {
    int32_t n = __cajeta_pb_tag(out, field, CAJ_PB_WIRE_BYTES);
    n += __cajeta_pb_varint(out + n, (uint64_t) len);
    for (int32_t i = 0; i < len; i++) out[n + i] = data[i];
    return n + len;
}

// ── trace assembly ────────────────────────────────────────────────────────
// Field numbers below are VERIFIED against third_party/perfetto/perfetto_trace.proto
// (PROVENANCE.md). A wrong one is valid protobuf that still does not load.
#define CAJ_PB_TRACE_PACKET        1    /* Trace.packet                        */
#define CAJ_PB_PKT_TIMESTAMP       8    /* TracePacket.timestamp               */
#define CAJ_PB_PKT_SEQ_ID         10    /* TracePacket.trusted_packet_sequence_id */
#define CAJ_PB_PKT_TRACK_EVENT    11    /* TracePacket.track_event             */
#define CAJ_PB_PKT_INTERNED       12    /* TracePacket.interned_data           */
#define CAJ_PB_PKT_SEQ_FLAGS      13    /* TracePacket.sequence_flags          */
#define CAJ_PB_PKT_TRACK_DESC     60    /* TracePacket.track_descriptor        */
#define CAJ_PB_TD_UUID             1    /* TrackDescriptor.uuid                */
#define CAJ_PB_TD_NAME             2    /* TrackDescriptor.name                */
#define CAJ_PB_TD_PARENT_UUID      5    /* TrackDescriptor.parent_uuid         */
#define CAJ_PB_TE_TYPE             9    /* TrackEvent.type                     */
#define CAJ_PB_TE_NAME_IID        10    /* TrackEvent.name_iid                 */
#define CAJ_PB_TE_TRACK_UUID      11    /* TrackEvent.track_uuid               */
#define CAJ_PB_TE_NAME            23    /* TrackEvent.name                     */
#define CAJ_PB_TE_SOURCE_LOC_IID  34    /* TrackEvent.source_location_iid      */
#define CAJ_PB_TE_FLOW_IDS        47    /* TrackEvent.flow_ids (fixed64)       */
#define CAJ_PB_TE_TERM_FLOW_IDS   48    /* TrackEvent.terminating_flow_ids     */
#define CAJ_PB_TE_DEBUG_ANNOS      4    /* TrackEvent.debug_annotations        */
#define CAJ_PB_DA_NAME            10    /* DebugAnnotation.name                */
#define CAJ_PB_DA_INT_VALUE        4    /* DebugAnnotation.int_value           */
#define CAJ_PB_DA_STRING_VALUE     6    /* DebugAnnotation.string_value        */
#define CAJ_PB_PKT_CLOCK_SNAP      6    /* TracePacket.clock_snapshot          */
#define CAJ_PB_CS_CLOCKS           1    /* ClockSnapshot.clocks                */
#define CAJ_PB_CLK_ID              1    /* ClockSnapshot.Clock.clock_id        */
#define CAJ_PB_CLK_TIMESTAMP       2    /* ClockSnapshot.Clock.timestamp       */
#define CAJ_PB_CLK_UNIT_MULT       4    /* ClockSnapshot.Clock.unit_multiplier_ns */
#define CAJ_BUILTIN_CLOCK_MONOTONIC 3   /* ClockSnapshot.Clock.BuiltinClocks   */
#define CAJ_PB_ID_EVENT_NAMES      2    /* InternedData.event_names            */
#define CAJ_PB_ID_SOURCE_LOCS      4    /* InternedData.source_locations       */
#define CAJ_PB_SL_IID              1    /* SourceLocation.iid                  */
#define CAJ_PB_SL_FILE             2    /* SourceLocation.file_name            */
#define CAJ_PB_SL_FUNCTION         3    /* SourceLocation.function_name        */
#define CAJ_PB_SL_LINE             4    /* SourceLocation.line_number          */
#define CAJ_PB_EN_IID              1    /* EventName.iid                       */
#define CAJ_PB_EN_NAME             2    /* EventName.name                      */

#define CAJ_TE_SLICE_BEGIN 1
#define CAJ_TE_SLICE_END   2
#define CAJ_TE_INSTANT     3

// TracePacket.SequenceFlags, a PAIR: CLEARED goes on the packet that ESTABLISHES
// incremental state (a sequence's first InternedData), NEEDS on every packet that
// CONSUMES it. A consumer packet missing NEEDS is skipped and loses its name.
#define CAJ_PB_SEQ_FLAG_CLEARED 1
#define CAJ_PB_SEQ_FLAG_NEEDS   2

// Bounded append buffer; `overflow` is sticky, so a short trace never reads whole.
typedef struct {
    uint8_t* buf;
    int32_t  cap;
    int32_t  len;
    int32_t  overflow;
} CajPbBuf;

static int32_t caj_pb_put(CajPbBuf* b, const uint8_t* d, int32_t n) {
    if (b->len + n > b->cap) { b->overflow = 1; return 0; }
    for (int32_t i = 0; i < n; i++) b->buf[b->len + i] = d[i];
    b->len += n;
    return n;
}

// Wraps a payload as one Trace.packet; the length prefix is what survives a cut.
static int32_t caj_pb_packet(CajPbBuf* out, const uint8_t* payload, int32_t len) {
    uint8_t hdr[16];
    int32_t n = __cajeta_pb_tag(hdr, CAJ_PB_TRACE_PACKET, CAJ_PB_WIRE_BYTES);
    n += __cajeta_pb_varint(hdr + n, (uint64_t) len);
    if (!caj_pb_put(out, hdr, n)) return 0;
    return caj_pb_put(out, payload, len) ? n + len : 0;
}

// TrackDescriptor: names a track and optionally parents it into a hierarchy.
int32_t __cajeta_prof_emit_track(CajPbBuf* out, uint64_t uuid,
                                 uint64_t parent_uuid, const char* name) {
    uint8_t td[512];
    int32_t n = __cajeta_pb_uint64(td, CAJ_PB_TD_UUID, uuid);
    if (name) {
        int32_t ln = 0; while (name[ln] && ln < 400) ln++;
        n += __cajeta_pb_bytes(td + n, CAJ_PB_TD_NAME, (const uint8_t*) name, ln);
    }
    if (parent_uuid) n += __cajeta_pb_uint64(td + n, CAJ_PB_TD_PARENT_UUID, parent_uuid);
    uint8_t pkt[600];
    int32_t p = __cajeta_pb_bytes(pkt, CAJ_PB_PKT_TRACK_DESC, td, n);
    return caj_pb_packet(out, pkt, p);
}

// InternedData carrying one EventName: a name is emitted once, then used by iid.
int32_t __cajeta_prof_emit_name(CajPbBuf* out, uint32_t seq_id,
                                uint64_t iid, const char* name,
                                int32_t first_in_sequence) {
    int32_t ln = 0; while (name && name[ln] && ln < 400) ln++;
    uint8_t en[512];
    int32_t e = __cajeta_pb_uint64(en, CAJ_PB_EN_IID, iid);
    e += __cajeta_pb_bytes(en + e, CAJ_PB_EN_NAME, (const uint8_t*) name, ln);
    uint8_t id[600];
    int32_t d = __cajeta_pb_bytes(id, CAJ_PB_ID_EVENT_NAMES, en, e);
    uint8_t pkt[700];
    int32_t p = __cajeta_pb_uint64(pkt, CAJ_PB_PKT_SEQ_ID, seq_id);
    // The interning table IS the incremental state, so CLEARED belongs here.
    p += __cajeta_pb_uint64(pkt + p, CAJ_PB_PKT_SEQ_FLAGS,
                            first_in_sequence ? CAJ_PB_SEQ_FLAG_CLEARED
                                              : CAJ_PB_SEQ_FLAG_NEEDS);
    p += __cajeta_pb_bytes(pkt + p, CAJ_PB_PKT_INTERNED, id, d);
    return caj_pb_packet(out, pkt, p);
}

// One TrackEvent: `name_iid` is an interned name (0 uses `name`), `extra` is
// pre-encoded TrackEvent bytes or NULL.
int32_t __cajeta_prof_emit_slice_anno(CajPbBuf* out, uint32_t seq_id, uint64_t ts,
                                      uint64_t track_uuid, int32_t type,
                                      uint64_t name_iid, const char* name,
                                      uint64_t source_iid, uint64_t flow_id,
                                      int32_t terminating,
                                      const uint8_t* extra, int32_t extraLen) {
    uint8_t te[768];
    int32_t n = __cajeta_pb_uint64(te, CAJ_PB_TE_TYPE, (uint64_t) type);
    n += __cajeta_pb_uint64(te + n, CAJ_PB_TE_TRACK_UUID, track_uuid);
    if (name_iid) {
        n += __cajeta_pb_uint64(te + n, CAJ_PB_TE_NAME_IID, name_iid);
    } else if (name) {
        int32_t ln = 0; while (name[ln] && ln < 400) ln++;
        n += __cajeta_pb_bytes(te + n, CAJ_PB_TE_NAME, (const uint8_t*) name, ln);
    }
    if (source_iid) n += __cajeta_pb_uint64(te + n, CAJ_PB_TE_SOURCE_LOC_IID, source_iid);
    // A flow needs BOTH ends; one end alone draws no arrow and still loads.
    if (flow_id)
        n += __cajeta_pb_fixed64(te + n,
                                 terminating ? CAJ_PB_TE_TERM_FLOW_IDS
                                             : CAJ_PB_TE_FLOW_IDS, flow_id);
    if (extra && extraLen > 0 && n + extraLen <= (int32_t) sizeof(te)) {
        memcpy(te + n, extra, (size_t) extraLen);
        n += extraLen;
    }
    uint8_t pkt[1024];
    int32_t p = __cajeta_pb_uint64(pkt, CAJ_PB_PKT_TIMESTAMP, ts);
    p += __cajeta_pb_uint64(pkt + p, CAJ_PB_PKT_SEQ_ID, seq_id);
    // Without NEEDS the reader skips the association and the name loads null.
    if (name_iid || source_iid)
        p += __cajeta_pb_uint64(pkt + p, CAJ_PB_PKT_SEQ_FLAGS, CAJ_PB_SEQ_FLAG_NEEDS);
    p += __cajeta_pb_bytes(pkt + p, CAJ_PB_PKT_TRACK_EVENT, te, n);
    return caj_pb_packet(out, pkt, p);
}

int32_t __cajeta_prof_emit_slice_flow(CajPbBuf* out, uint32_t seq_id, uint64_t ts,
                                      uint64_t track_uuid, int32_t type,
                                      uint64_t name_iid, const char* name,
                                      uint64_t source_iid, uint64_t flow_id,
                                      int32_t terminating) {
    return __cajeta_prof_emit_slice_anno(out, seq_id, ts, track_uuid, type,
                                         name_iid, name, source_iid, flow_id,
                                         terminating, NULL, 0);
}

// The flow-free form every non-GPU caller uses; one implementation for both.
int32_t __cajeta_prof_emit_slice(CajPbBuf* out, uint32_t seq_id, uint64_t ts,
                                 uint64_t track_uuid, int32_t type,
                                 uint64_t name_iid, const char* name,
                                 uint64_t source_iid) {
    return __cajeta_prof_emit_slice_flow(out, seq_id, ts, track_uuid, type,
                                         name_iid, name, source_iid, 0, 0);
}

// ── streaming writer + interning table (5.2.c, 5.2.d) ─────────────────────
// Packets are appended as they are built, never accumulated; one scratch buffer.
#define CAJ_PROF_MAX_INTERNED 4096
#define CAJ_PROF_SCRATCH 4096
#define CAJ_PROF_NAME_POOL (256 * 1024)

typedef struct {
    FILE*       f;
    uint32_t    seq_id;
    int32_t     n_names;
    // The table OWNS its names; a caller's buffer may hold the next name already.
    int32_t     name_off[CAJ_PROF_MAX_INTERNED];
    // Source locations intern in their OWN iid space: one EventName, many of these.
    int32_t     n_srcs;
    int32_t     src_file_off[CAJ_PROF_MAX_INTERNED];
    int32_t     src_func_off[CAJ_PROF_MAX_INTERNED];
    int32_t     src_line[CAJ_PROF_MAX_INTERNED];
    int32_t     pool_used;
    char        pool[CAJ_PROF_NAME_POOL];
    int32_t     state_cleared;   // has a CLEARED packet been emitted yet
    int64_t     packets;
    int64_t     bytes;
    uint8_t     scratch[CAJ_PROF_SCRATCH];
} CajProfWriter;

static int32_t caj_prof_flush(CajProfWriter* w, CajPbBuf* b) {
    if (b->overflow) return 0;      // never write a half packet
    if (b->len <= 0) return 0;
    size_t n = fwrite(b->buf, 1, (size_t) b->len, w->f);
    if (n != (size_t) b->len) return 0;
    w->packets++;
    w->bytes += b->len;
    return b->len;
}

// Opens `path` for writing and resets the interning tables and counters.
int32_t __cajeta_prof_trace_open(CajProfWriter* w, const char* path) {
    if (!w || !path) return 0;
    w->f = fopen(path, "wb");
    if (!w->f) return 0;
    w->seq_id = 1;
    w->n_names = 0;
    w->n_srcs = 0;
    w->pool_used = 0;
    w->state_cleared = 0;
    w->packets = 0;
    w->bytes = 0;
    return 1;
}

// Returns the iid for `name`, emitting an InternedData packet the first time it is
// seen; 0 means "not interned". Compared by CONTENT, never by pointer.
uint64_t __cajeta_prof_intern(CajProfWriter* w, const char* name) {
    if (!w || !name) return 0;
    for (int32_t i = 0; i < w->n_names; i++) {
        const char* c = &w->pool[w->name_off[i]];
        const char* d = name;
        while (*c && *c == *d) { c++; d++; }
        if (*c == 0 && *d == 0) return (uint64_t) (i + 1);
    }
    if (w->n_names >= CAJ_PROF_MAX_INTERNED) return 0;   // fall back to inline
    int32_t len = 0;
    while (name[len]) len++;
    if (w->pool_used + len + 1 > CAJ_PROF_NAME_POOL) return 0;  // fall back
    int32_t off = w->pool_used;
    for (int32_t k = 0; k <= len; k++) w->pool[off + k] = name[k];
    w->pool_used += len + 1;
    w->name_off[w->n_names++] = off;
    uint64_t iid = (uint64_t) w->n_names;

    CajPbBuf b = { w->scratch, CAJ_PROF_SCRATCH, 0, 0 };
    __cajeta_prof_emit_name(&b, w->seq_id, iid, name, w->state_cleared ? 0 : 1);
    w->state_cleared = 1;
    if (!caj_prof_flush(w, &b)) { w->n_names--; w->pool_used = off; return 0; }
    return iid;
}

// Copies a string into the writer's pool, returning its offset or -1.
static int32_t caj_prof_pool_put(CajProfWriter* w, const char* s) {
    int32_t len = 0;
    while (s[len]) len++;
    if (w->pool_used + len + 1 > CAJ_PROF_NAME_POOL) return -1;
    int32_t off = w->pool_used;
    for (int32_t k = 0; k <= len; k++) w->pool[off + k] = s[k];
    w->pool_used += len + 1;
    return off;
}

static int32_t caj_prof_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

// SourceLocation packet: file, function, line. Emitted once per distinct triple.
static int32_t caj_prof_emit_source(CajProfWriter* w, uint64_t iid,
                                    const char* file, const char* func,
                                    int32_t line, int32_t first) {
    uint8_t sl[600];
    int32_t e = __cajeta_pb_uint64(sl, CAJ_PB_SL_IID, iid);
    int32_t fl = 0; while (file && file[fl]) fl++;
    e += __cajeta_pb_bytes(sl + e, CAJ_PB_SL_FILE, (const uint8_t*) file, fl);
    int32_t nl = 0; while (func && func[nl]) nl++;
    e += __cajeta_pb_bytes(sl + e, CAJ_PB_SL_FUNCTION, (const uint8_t*) func, nl);
    e += __cajeta_pb_uint64(sl + e, CAJ_PB_SL_LINE, (uint64_t) (line > 0 ? line : 0));
    uint8_t id[700];
    int32_t d = __cajeta_pb_bytes(id, CAJ_PB_ID_SOURCE_LOCS, sl, e);
    uint8_t pkt[800];
    int32_t p = __cajeta_pb_uint64(pkt, CAJ_PB_PKT_SEQ_ID, w->seq_id);
    p += __cajeta_pb_uint64(pkt + p, CAJ_PB_PKT_SEQ_FLAGS,
                            first ? CAJ_PB_SEQ_FLAG_CLEARED : CAJ_PB_SEQ_FLAG_NEEDS);
    p += __cajeta_pb_bytes(pkt + p, CAJ_PB_PKT_INTERNED, id, d);
    CajPbBuf b = { w->scratch, CAJ_PROF_SCRATCH, 0, 0 };
    // caj_pb_packet, NOT caj_pb_put: unframed bytes leave iids that never resolve.
    caj_pb_packet(&b, pkt, p);
    return caj_prof_flush(w, &b);
}

// Interns a (file, function, line) triple; 0 means the caller omits the field.
uint64_t __cajeta_prof_intern_source(CajProfWriter* w, const char* file,
                                     const char* func, int32_t line) {
    if (!w || !file || !func) return 0;
    for (int32_t i = 0; i < w->n_srcs; i++) {
        if (w->src_line[i] == line
            && caj_prof_streq(&w->pool[w->src_file_off[i]], file)
            && caj_prof_streq(&w->pool[w->src_func_off[i]], func))
            return (uint64_t) (i + 1);
    }
    if (w->n_srcs >= CAJ_PROF_MAX_INTERNED) return 0;
    int32_t fo = caj_prof_pool_put(w, file);
    if (fo < 0) return 0;
    int32_t no = caj_prof_pool_put(w, func);
    if (no < 0) { w->pool_used = fo; return 0; }
    w->src_file_off[w->n_srcs] = fo;
    w->src_func_off[w->n_srcs] = no;
    w->src_line[w->n_srcs] = line;
    w->n_srcs++;
    uint64_t iid = (uint64_t) w->n_srcs;
    if (!caj_prof_emit_source(w, iid, file, func, line, w->state_cleared ? 0 : 1)) {
        w->n_srcs--; w->pool_used = fo; return 0;
    }
    w->state_cleared = 1;
    return iid;
}

int32_t __cajeta_prof_trace_source_count(CajProfWriter* w) { return w ? w->n_srcs : 0; }

// Emits one TrackDescriptor packet for `uuid`, optionally parented.
int32_t __cajeta_prof_trace_track(CajProfWriter* w, uint64_t uuid,
                                  uint64_t parent_uuid, const char* name) {
    if (!w || !w->f) return 0;
    CajPbBuf b = { w->scratch, CAJ_PROF_SCRATCH, 0, 0 };
    __cajeta_prof_emit_track(&b, uuid, parent_uuid, name);
    return caj_prof_flush(w, &b);
}

// Interns `name` and emits the slice referencing it; `file` and `line` are optional.
// The line is the one seen WHEN THE SLICE OPENED, not where the frame spent time.
int32_t __cajeta_prof_trace_slice_at(CajProfWriter* w, uint64_t ts,
                                     uint64_t track_uuid, int32_t type,
                                     const char* name, const char* file,
                                     int32_t line) {
    if (!w || !w->f) return 0;
    uint64_t iid = name ? __cajeta_prof_intern(w, name) : 0;
    uint64_t src = (file && name) ? __cajeta_prof_intern_source(w, file, name, line) : 0;
    CajPbBuf b = { w->scratch, CAJ_PROF_SCRATCH, 0, 0 };
    // iid 0 with a non-NULL name means interning failed; fall back to inline.
    __cajeta_prof_emit_slice(&b, w->seq_id, ts, track_uuid, type,
                             iid, iid ? NULL : name, src);
    return caj_prof_flush(w, &b);
}

int32_t __cajeta_prof_trace_slice(CajProfWriter* w, uint64_t ts,
                                  uint64_t track_uuid, int32_t type,
                                  const char* name) {
    return __cajeta_prof_trace_slice_at(w, ts, track_uuid, type, name, NULL, 0);
}

int64_t __cajeta_prof_trace_packets(CajProfWriter* w) { return w ? w->packets : 0; }
int64_t __cajeta_prof_trace_bytes(CajProfWriter* w)   { return w ? w->bytes : 0; }
int32_t __cajeta_prof_trace_interned(CajProfWriter* w){ return w ? w->n_names : 0; }
int32_t __cajeta_prof_trace_writer_size(void)         { return (int32_t) sizeof(CajProfWriter); }

// Flushes and closes. There is no footer, which is why a killed run still reads.
int32_t __cajeta_prof_trace_close(CajProfWriter* w) {
    if (!w || !w->f) return 0;
    int r = fflush(w->f) == 0;
    fclose(w->f);
    w->f = NULL;
    return r;
}

// ── Unit 6: samples become slices ─────────────────────────────────────────
// A sampler produces periodic STACKS; TrackEvent wants SLICES. Each sample is diffed
// against the open stack, and every slice boundary lands on a SAMPLE TICK.
#define CAJ_PROF_MAX_TRACKS 256
#define CAJ_PROF_MAX_DEPTH  CAJETA_PROF_MAX_FRAMES

typedef struct {
    void*   owner;
    int32_t kind;
    int64_t id;                                 // captured at SAMPLE time
    uint64_t uuid;
    int32_t depth;                              // frames currently open
    const CajetaFrameDesc* open[CAJ_PROF_MAX_DEPTH];   // outermost -> innermost
} CajProfTrack;

// Stable track names; a fiber gets the same debugger id a DAP session shows.
static void caj_prof_track_name(char* out, int32_t cap, const CajProfTrack* t,
                                int32_t index) {
    const char* kind = (t->kind == CAJETA_PROF_OWNER_FIBER) ? "fiber" : "thread";
    // The fiber id comes from the SAMPLE: at drain the handle is long dead.
    long id = (t->kind == CAJETA_PROF_OWNER_FIBER) ? (long) t->id : (long) index;
    snprintf(out, (size_t) cap, "cajeta.%s.%ld", kind, id);
}

// Returns the track for `owner`, emitting its TrackDescriptor on first sight.
static CajProfTrack* caj_prof_track_for(CajProfTrack* tracks, int32_t* n,
                                        CajProfWriter* w, void* owner,
                                        int32_t kind, int64_t id) {
    for (int32_t i = 0; i < *n; i++)
        if (tracks[i].owner == owner) return &tracks[i];
    if (*n >= CAJ_PROF_MAX_TRACKS) return NULL;
    CajProfTrack* t = &tracks[*n];
    t->owner = owner;
    t->kind = kind;
    t->id = id;
    t->depth = 0;
    // The handle's address is a stable non-zero uuid, and is never dereferenced.
    t->uuid = (uint64_t) (uintptr_t) owner;
    char name[64];
    caj_prof_track_name(name, (int32_t) sizeof(name), t, *n);
    __cajeta_prof_trace_track(w, t->uuid, 0, name);
    (*n)++;
    return t;
}

// One frame's display name, "Type.method", built into the caller's buffer.
static void caj_prof_frame_name(char* out, int32_t cap, const CajetaFrameDesc* d) {
    const char* t = (d && d->typeName) ? d->typeName : "?";
    const char* m = (d && d->methodName) ? d->methodName : "?";
    snprintf(out, (size_t) cap, "%s.%s", t, m);
}

// Defined below, next to the annotation helpers they use.
int32_t __cajeta_prof_trace_metadata(CajProfWriter* w, uint64_t ts,
                                     const char* tier, int32_t rate_hz,
                                     int32_t ring_cap, int64_t samples,
                                     int64_t dropped, int64_t frames);
int64_t __cajeta_prof_instr_to_trace(CajProfWriter* w, uint64_t ts);

typedef struct {
    const char* tier;
    int32_t     rate_hz;
    int32_t     ring_cap;
    int64_t     samples;
    int64_t     dropped;
    int64_t     frames;
} CajProfMeta;

// Defined in cajeta_rt_prof_gpu.c, later in this TU (6.6).
int64_t __cajeta_prof_gpu_captured_to_trace(CajProfWriter* w, uint64_t ts);
void    __cajeta_prof_gpu_capture_settle(void);

// Converts an ordered run of samples into a trace at `path`, stamping run metadata
// when `meta` is given. Samples are a parameter so tracegen drives this exact code.
int64_t __cajeta_prof_samples_to_trace_meta(const CajetaProfSample* samples,
                                            int64_t n, const char* path,
                                            const CajProfMeta* meta) {
    if (!samples || n <= 0) return 0;
    static CajProfWriter w;
    if (!__cajeta_prof_trace_open(&w, path)) return 0;
    // Settle the GPU side FIRST, or the metadata below reports zero ROCm records.
    __cajeta_prof_gpu_capture_settle();
    if (meta)
        __cajeta_prof_trace_metadata(&w, (uint64_t) samples[0].host_ns,
                                     meta->tier, meta->rate_hz, meta->ring_cap,
                                     meta->samples, meta->dropped, meta->frames);

    static CajProfTrack tracks[CAJ_PROF_MAX_TRACKS];
    int32_t n_tracks = 0;
    int64_t last_ts = 0;
    for (int64_t i = 0; i < n; i++) {
        const CajetaProfSample* s = &samples[i];
        CajProfTrack* t = caj_prof_track_for(tracks, &n_tracks, &w,
                                             s->owner, s->owner_kind,
                                             s->owner_id);
        if (!t) continue;
        last_ts = s->host_ns;

        // The snapshot is innermost-first; slices nest outermost-first.
        int32_t n = s->n_frames;
        if (n > CAJ_PROF_MAX_DEPTH) n = CAJ_PROF_MAX_DEPTH;
        const CajetaFrameDesc* cur[CAJ_PROF_MAX_DEPTH];
        int32_t curline[CAJ_PROF_MAX_DEPTH];
        for (int32_t k = 0; k < n; k++) {
            cur[k] = s->frames[n - 1 - k].desc;
            curline[k] = s->frames[n - 1 - k].line;
        }

        int32_t common = 0;
        while (common < n && common < t->depth && cur[common] == t->open[common])
            common++;

        // Close what vanished, innermost first: SLICE_END must reverse BEGIN.
        for (int32_t k = t->depth - 1; k >= common; k--)
            __cajeta_prof_trace_slice(&w, (uint64_t) s->host_ns, t->uuid,
                                      CAJ_TE_SLICE_END, NULL);
        for (int32_t k = common; k < n; k++) {
            char name[256];
            caj_prof_frame_name(name, (int32_t) sizeof(name), cur[k]);
            const char* file = (cur[k] && cur[k]->fileName) ? cur[k]->fileName : NULL;
            __cajeta_prof_trace_slice_at(&w, (uint64_t) s->host_ns, t->uuid,
                                         CAJ_TE_SLICE_BEGIN, name, file, curline[k]);
            t->open[k] = cur[k];
        }
        t->depth = n;
    }

    // Close every slice still open, or the trace ends unterminated.
    for (int32_t i = 0; i < n_tracks; i++)
        for (int32_t k = tracks[i].depth - 1; k >= 0; k--)
            __cajeta_prof_trace_slice(&w, (uint64_t) last_ts, tracks[i].uuid,
                                      CAJ_TE_SLICE_END, NULL);

    // Instrumentation records go in the SAME trace, on their own track.
    __cajeta_prof_instr_to_trace(&w, (uint64_t) last_ts);

    __cajeta_prof_gpu_captured_to_trace(&w, (uint64_t) last_ts);

    int64_t packets = __cajeta_prof_trace_packets(&w);
    __cajeta_prof_trace_close(&w);
    return packets;
}

// An instrumented run with no sampler still has a profile; the ring drain returns
// early on an empty ring, so this path is its only writer.
int64_t __cajeta_prof_instr_only_to_trace(const char* path) {
    if (!__cajeta_prof_instr_is_present()) return 0;
    if (__cajeta_prof_instr_method_count() <= 0) return 0;
    static CajProfWriter w;
    if (!__cajeta_prof_trace_open(&w, path)) return 0;
    // Timestamp 0: these are run totals, and "now" would postdate the work.
    __cajeta_prof_trace_metadata(&w, 0, "instrumentation", 0, 0, 0, 0, 0);
    __cajeta_prof_instr_to_trace(&w, 0);
    int64_t packets = __cajeta_prof_trace_packets(&w);
    __cajeta_prof_trace_close(&w);
    return packets;
}

// ── GPU dispatch records -> trace (Unit 7; spec §7.2, §7.3) ───────────────
// Beside the sample transform and standalone-compilable, so tracegen drives it.

// Track uuids; the top nibble is set so one never collides with a HOST track.
#define CAJ_GPU_UUID_DEVICE  (0xD000ULL << 48)
#define CAJ_GPU_UUID_CONTEXT (0xC000ULL << 48)
#define CAJ_GPU_UUID_QUEUE   (0x9000ULL << 48)

static uint64_t caj_gpu_uuid(uint64_t base, int32_t backend, int32_t device,
                             int64_t queue) {
    return base
         | ((uint64_t) (backend & 0xF) << 40)
         | ((uint64_t) (device & 0xFF) << 32)
         | (uint64_t) ((uint32_t) queue);
}

// Mirrors the backend enum in cajeta_xpu_dispatch.c; presentation only.
static const char* caj_gpu_backend_name(int32_t backend) {
    switch (backend) {
        case CAJ_GPU_BACKEND_CUDA:   return "cuda";
        case CAJ_GPU_BACKEND_HIP:    return "hip";
        case CAJ_GPU_BACKEND_VULKAN: return "vulkan";
        case CAJ_GPU_BACKEND_CPU:    return "cpu";
        default: return "xpu";
    }
}

#define CAJ_GPU_MAX_TRACKS 64

// Track descriptors already emitted; held by the caller so batches never redeclare.
typedef struct {
    uint64_t uuid[CAJ_GPU_MAX_TRACKS];
    int32_t  n;
} CajGpuTracks;

int32_t __cajeta_prof_gpu_tracks_size(void) { return (int32_t) sizeof(CajGpuTracks); }

static int32_t caj_gpu_track_seen(CajGpuTracks* t, uint64_t uuid) {
    for (int32_t i = 0; i < t->n; i++) if (t->uuid[i] == uuid) return 1;
    if (t->n < CAJ_GPU_MAX_TRACKS) t->uuid[t->n++] = uuid;
    return 0;
}

// Defined with the metadata writer below.
static int32_t caj_prof_anno_int(uint8_t* out, const char* name, int64_t v);

// One ClockSnapshot pairing the host clock with a device domain, so a reader can
// reproduce the mapping. The device clock id is sequence-scoped and its timestamp
// is in TICKS (unit_multiplier_ns 1); the correlation carries the period.
int32_t __cajeta_prof_trace_clock_snapshot(CajProfWriter* w, int32_t domain,
                                           int64_t hostNs, int64_t devTicks) {
    if (!w) return 0;
    uint8_t hostClk[32];
    int32_t hc = __cajeta_pb_uint64(hostClk, CAJ_PB_CLK_ID,
                                    CAJ_BUILTIN_CLOCK_MONOTONIC);
    hc += __cajeta_pb_uint64(hostClk + hc, CAJ_PB_CLK_TIMESTAMP,
                             (uint64_t) hostNs);

    uint8_t devClk[32];
    int32_t dc = __cajeta_pb_uint64(devClk, CAJ_PB_CLK_ID,
                                    (uint64_t) (CAJETA_CLOCK_PERFETTO_BASE_ID + domain));
    dc += __cajeta_pb_uint64(devClk + dc, CAJ_PB_CLK_TIMESTAMP,
                             (uint64_t) devTicks);
    dc += __cajeta_pb_uint64(devClk + dc, CAJ_PB_CLK_UNIT_MULT, 1);

    uint8_t cs[96];
    int32_t c = __cajeta_pb_bytes(cs, CAJ_PB_CS_CLOCKS, hostClk, hc);
    c += __cajeta_pb_bytes(cs + c, CAJ_PB_CS_CLOCKS, devClk, dc);

    uint8_t pkt[160];
    int32_t p = __cajeta_pb_uint64(pkt, CAJ_PB_PKT_SEQ_ID, w->seq_id);
    p += __cajeta_pb_bytes(pkt + p, CAJ_PB_PKT_CLOCK_SNAP, cs, c);

    CajPbBuf b = { w->scratch, CAJ_PROF_SCRATCH, 0, 0 };
    caj_pb_packet(&b, pkt, p);
    caj_prof_flush(w, &b);
    return 1;
}

// Emits `n` dispatch records into an open writer: clock snapshots, then a launch
// instant and a device slice per event. Returns packets written.
int64_t __cajeta_prof_gpu_emit(CajProfWriter* w, CajGpuTracks* seen,
                               const CajetaGpuEvent* evs, int32_t n) {
    if (!w || !seen || !evs || n <= 0) return 0;
    int64_t before = __cajeta_prof_trace_packets(w);

    // Every calibration replayed BEFORE the spans that depend on it.
    {
        int32_t snaps = __cajeta_prof_clock_snapshot_count();
        for (int32_t i = 0; i < snaps; i++) {
            CajetaClockSnapshot cs;
            if (__cajeta_prof_clock_snapshot_get(i, &cs)) {
                __cajeta_prof_trace_clock_snapshot(w, cs.domain, cs.hostNs,
                                                   cs.devTicks);
            }
        }
    }
    for (int32_t i = 0; i < n; i++) {
        const CajetaGpuEvent* e = &evs[i];
        const char* bname = caj_gpu_backend_name(e->backend);
        uint64_t dev = caj_gpu_uuid(CAJ_GPU_UUID_DEVICE,  e->backend, e->device_id, 0);
        uint64_t ctx = caj_gpu_uuid(CAJ_GPU_UUID_CONTEXT, e->backend, e->device_id, 0);
        uint64_t que = caj_gpu_uuid(CAJ_GPU_UUID_QUEUE,   e->backend, e->device_id, e->queue);
        char nm[128];

        // device -> context -> queue is a real hierarchy; flat roots go wrong on two.
        if (!caj_gpu_track_seen(seen, dev)) {
            snprintf(nm, sizeof(nm), "cajeta.xpu.%s device %d", bname, e->device_id);
            __cajeta_prof_trace_track(w, dev, 0, nm);
        }
        if (!caj_gpu_track_seen(seen, ctx)) {
            snprintf(nm, sizeof(nm), "context %d", e->device_id);
            __cajeta_prof_trace_track(w, ctx, dev, nm);
        }
        if (!caj_gpu_track_seen(seen, que)) {
            snprintf(nm, sizeof(nm), "queue %lld", (long long) e->queue);
            __cajeta_prof_trace_track(w, que, ctx, nm);
        }
        // The launching thread's track; its uuid is the handle address, which a
        // later merge with the sampler already agrees on.
        uint64_t host = (uint64_t) (uintptr_t) e->host_thread;
        if (host && !caj_gpu_track_seen(seen, host)) {
            snprintf(nm, sizeof(nm), "cajeta.thread.%llu", (unsigned long long) host);
            __cajeta_prof_trace_track(w, host, 0, nm);
        }

        const char* kn = e->kernel_name ? e->kernel_name : "?";
        // Host launch site: an instant carrying the flow id and source location.
        if (host) {
            const CajetaFrameDesc* d = e->call_site;
            uint64_t iid = __cajeta_prof_intern(w, kn);
            // "Type.method" via the SAME helper the sampler uses, or one site
            // ends up with two names.
            char qual[192];
            if (d) caj_prof_frame_name(qual, (int32_t) sizeof(qual), d);
            uint64_t src = (d && d->fileName)
                ? __cajeta_prof_intern_source(w, d->fileName, qual,
                                              e->call_site_line)
                : 0;
            CajPbBuf b = { w->scratch, CAJ_PROF_SCRATCH, 0, 0 };
            __cajeta_prof_emit_slice_flow(&b, w->seq_id, (uint64_t) e->host_launch_ns,
                                          host, CAJ_TE_INSTANT, iid, iid ? NULL : kn,
                                          src, (uint64_t) e->launch_id, 0);
            caj_prof_flush(w, &b);
        }
        // Device execution: a slice on the queue track, terminating the flow. The
        // tier, clock confidence and integrity flags ride on the measurement
        // itself - one run can mix them, and a flagged span still renders.
        {
            uint64_t iid = __cajeta_prof_intern(w, kn);
            uint8_t anno[256];
            int32_t a = caj_prof_anno_int(anno, "tier", e->tier);
            a += caj_prof_anno_int(anno + a, "clock_confidence",
                                   __cajeta_prof_clock_confidence(e->backend));
            // The checker's flags OR'd with what only the producer can know.
            int32_t integrity = __cajeta_prof_check_dispatch(e)
                              | e->integrity_flags;
            if (integrity != CAJETA_SPAN_OK) {
                a += caj_prof_anno_int(anno + a, "integrity_flags", integrity);
            }
            CajPbBuf b = { w->scratch, CAJ_PROF_SCRATCH, 0, 0 };
            __cajeta_prof_emit_slice_anno(&b, w->seq_id, (uint64_t) e->dev_start_ns,
                                          que, CAJ_TE_SLICE_BEGIN, iid, iid ? NULL : kn,
                                          0, (uint64_t) e->launch_id, 1, anno, a);
            caj_prof_flush(w, &b);
        }
        __cajeta_prof_trace_slice(w, (uint64_t) e->dev_end_ns, que,
                                  CAJ_TE_SLICE_END, NULL);
    }
    return __cajeta_prof_trace_packets(w) - before;
}

// One-shot form: open, emit, close. What tracegen drives.
int64_t __cajeta_prof_gpu_events_to_trace(const CajetaGpuEvent* evs, int64_t n,
                                          const char* path) {
    if (!evs || n <= 0) return 0;
    static CajProfWriter w;
    static CajGpuTracks seen;
    seen.n = 0;
    if (!__cajeta_prof_trace_open(&w, path)) return 0;
    // The run record's only home on this path; the sampler counters are honestly
    // zero, which beats borrowing a count that means something else.
    __cajeta_prof_trace_metadata(&w, (uint64_t) evs[0].host_launch_ns,
                                 "device", 0, 0, 0, 0, 0);
    __cajeta_prof_gpu_emit(&w, &seen, evs, (int32_t) n);
    __cajeta_prof_trace_close(&w);
    return __cajeta_prof_trace_packets(&w);
}

int64_t __cajeta_prof_samples_to_trace(const CajetaProfSample* samples,
                                       int64_t n, const char* path) {
    return __cajeta_prof_samples_to_trace_meta(samples, n, path, NULL);
}

// ── 6.2.c / spec §7.8: what produced this trace ──────────────────────────
// One DebugAnnotation, int or string form; returns the bytes written.
static int32_t caj_prof_anno_int(uint8_t* out, const char* name, int64_t v) {
    uint8_t da[128];
    int32_t ln = 0; while (name[ln]) ln++;
    int32_t d = __cajeta_pb_bytes(da, CAJ_PB_DA_NAME, (const uint8_t*) name, ln);
    d += __cajeta_pb_uint64(da + d, CAJ_PB_DA_INT_VALUE, (uint64_t) v);
    return __cajeta_pb_bytes(out, CAJ_PB_TE_DEBUG_ANNOS, da, d);
}
static int32_t caj_prof_anno_str(uint8_t* out, const char* name, const char* v) {
    uint8_t da[256];
    int32_t ln = 0; while (name[ln]) ln++;
    int32_t vl = 0; while (v[vl]) vl++;
    int32_t d = __cajeta_pb_bytes(da, CAJ_PB_DA_NAME, (const uint8_t*) name, ln);
    d += __cajeta_pb_bytes(da + d, CAJ_PB_DA_STRING_VALUE, (const uint8_t*) v, vl);
    return __cajeta_pb_bytes(out, CAJ_PB_TE_DEBUG_ANNOS, da, d);
}

// Per-domain calibration quality, driver identity and active layers, for domains
// that actually calibrated - an uncalibrated run must not invent a figure. Drift
// is integer MILLI-ppm; whole ppm would erase a sub-ppm device.
static int32_t caj_prof_calibration_annos(uint8_t* out, int32_t cap) {
    int32_t n = 0;
    int32_t calibrated = 0;
    for (int32_t d = 0; d < CAJETA_CLOCK_MAX_DOMAINS; d++) {
        if (__cajeta_prof_clock_valid(d)) calibrated++;
    }
    if (cap < 32) return 0;
    n += caj_prof_anno_int(out + n, "clock_domains_calibrated", calibrated);

    for (int32_t d = 0; d < CAJETA_CLOCK_MAX_DOMAINS; d++) {
        if (!__cajeta_prof_clock_valid(d)) continue;
        // Stop before the buffer rather than truncate an annotation mid-field.
        if (cap - n < 320) break;
        char key[48];
        snprintf(key, sizeof(key), "clock%d_confidence", d);
        n += caj_prof_anno_int(out + n, key, __cajeta_prof_clock_confidence(d));
        snprintf(key, sizeof(key), "clock%d_drift_ppm_milli", d);
        n += caj_prof_anno_int(out + n, key,
                               (int64_t) llround(__cajeta_prof_clock_drift_ppm(d) * 1000.0));
        snprintf(key, sizeof(key), "clock%d_offset_ns", d);
        n += caj_prof_anno_int(out + n, key, __cajeta_prof_clock_offset_ns(d));
        snprintf(key, sizeof(key), "clock%d_samples", d);
        n += caj_prof_anno_int(out + n, key, __cajeta_prof_clock_samples(d));
        snprintf(key, sizeof(key), "clock%d_rejected", d);
        n += caj_prof_anno_int(out + n, key, __cajeta_prof_clock_rejected(d));
        snprintf(key, sizeof(key), "clock%d_recalibrations", d);
        n += caj_prof_anno_int(out + n, key, __cajeta_prof_clock_generation(d));
        snprintf(key, sizeof(key), "clock%d_tier", d);
        n += caj_prof_anno_int(out + n, key, __cajeta_prof_tier(d));
        snprintf(key, sizeof(key), "clock%d_demote_reason", d);
        n += caj_prof_anno_int(out + n, key, __cajeta_prof_tier_reason(d));

        const char* drv = __cajeta_prof_driver_identity(d);
        if (drv) {
            snprintf(key, sizeof(key), "clock%d_driver", d);
            n += caj_prof_anno_str(out + n, key, drv);
        }
        const char* lay = __cajeta_prof_active_layers(d);
        if (lay) {
            snprintf(key, sizeof(key), "clock%d_layers", d);
            n += caj_prof_anno_str(out + n, key, lay);
        }
    }
    return n;
}

#ifndef CAJETA_PROF_TRACE_STANDALONE
// ── Unit 8 — what the ROCm backend actually did (§5.2, §6.4) ─────────────
// Present on every trace that ATTEMPTED ROCm, especially where it did not work:
// a degraded trace must not look device-timed.
static int32_t caj_prof_rocm_annos(uint8_t* out, int32_t cap) {
    int32_t n = 0;
    const int32_t state = __cajeta_prof_rocm_state();
    if (state == CAJETA_ROCM_UNATTEMPTED) return 0;   // no GPU run; say nothing
    if (cap < 640) return 0;

    n += caj_prof_anno_int(out + n, "rocm_state", state);
    n += caj_prof_anno_int(out + n, "rocm_tracing", __cajeta_prof_rocm_tracing());
    n += caj_prof_anno_int(out + n, "rocm_launches", __cajeta_prof_rocm_launches());
    n += caj_prof_anno_int(out + n, "rocm_records", __cajeta_prof_rocm_records());
    // Records matching no launch of ours (HIP's own kernels), reported not hidden.
    n += caj_prof_anno_int(out + n, "rocm_unmatched_records",
                           __cajeta_prof_rocm_unmatched());
    n += caj_prof_anno_int(out + n, "rocm_clock_offset_ns",
                           __cajeta_prof_rocm_clock_offset_ns());
    // A trace spanning a suspend renders perfectly, everything after it displaced.
    n += caj_prof_anno_int(out + n, "rocm_suspended", __cajeta_prof_rocm_suspended());
    if (__cajeta_prof_rocm_suspended())
        n += caj_prof_anno_int(out + n, "rocm_suspend_ns",
                               __cajeta_prof_rocm_suspend_ns());
    if (state != CAJETA_ROCM_READY) {
        const char* why = __cajeta_prof_rocm_reason();
        if (why && *why) n += caj_prof_anno_str(out + n, "rocm_degraded_reason", why);
    }
    return n;
}
#endif

// What an instrumented build cost, left out, and was optimized at. Emitted only
// where probes exist: a zero-valued record would read as "the probes found none".
static int32_t caj_prof_instr_annos(uint8_t* out, int32_t cap) {
    if (!__cajeta_prof_instr_is_present()) return 0;
    if (cap < 512) return 0;
    int32_t n = 0;
    n += caj_prof_anno_str(out + n, "instr_tier", "instrumentation");
    n += caj_prof_anno_int(out + n, "instr_methods",
                           __cajeta_prof_instr_method_count());
    n += caj_prof_anno_int(out + n, "instr_calls",
                           __cajeta_prof_instr_total_calls());
    n += caj_prof_anno_int(out + n, "instr_probe_pairs",
                           __cajeta_prof_instr_probe_pairs());
    n += caj_prof_anno_int(out + n, "instr_probe_ns",
                           __cajeta_prof_instr_probe_ns());
    n += caj_prof_anno_int(out + n, "instr_overhead_ns",
                           __cajeta_prof_instr_overhead_ns());
    // The flag pins no optimization level, so the level is part of what these mean.
    n += caj_prof_anno_int(out + n, "instr_opt_level",
                           __cajeta_prof_instr_opt_level());
    // A profile that silently omits code reads as though that code were free.
    const char* sel = __cajeta_prof_instr_selection();
    n += caj_prof_anno_str(out + n, "instr_selection",
                           (sel && sel[0]) ? sel : "all");
    return n;
}

// The instrumentation track: one INSTANT per method, each carrying `source`, so a
// consumer can always say which tier produced a number.
#define CAJ_INSTR_TRACK_UUID (0x1A000ULL << 44)

int64_t __cajeta_prof_instr_to_trace(CajProfWriter* w, uint64_t ts) {
    if (!w || !w->f) return 0;
    const int32_t n_methods = __cajeta_prof_instr_method_count();
    if (n_methods <= 0) return 0;
    const int64_t before = __cajeta_prof_trace_packets(w);
    __cajeta_prof_trace_track(w, CAJ_INSTR_TRACK_UUID, 0, "cajeta.instrumentation");

    for (int32_t i = 0; i < n_methods; i++) {
        char name[256];
        const char* ty = __cajeta_prof_instr_method_type(i);
        const char* mn = __cajeta_prof_instr_method_name(i);
        const char* fl = __cajeta_prof_instr_method_file(i);
        snprintf(name, sizeof(name), "%s%s%s(%s)",
                 ty ? ty : "", (ty && ty[0]) ? "." : "", mn ? mn : "?",
                 fl ? fl : "");
        uint64_t iid = __cajeta_prof_intern(w, name);
        uint64_t src = (fl && fl[0] && mn) ? __cajeta_prof_intern_source(w, fl, mn, 0) : 0;

        uint8_t extra[512];
        int32_t e = 0;
        e += caj_prof_anno_str(extra + e, "source", "instrumentation");
        e += caj_prof_anno_int(extra + e, "calls",
                               __cajeta_prof_instr_method_calls(i));
        e += caj_prof_anno_int(extra + e, "inclusive_ns",
                               __cajeta_prof_instr_method_inclusive_ns(i));
        // Entries reached with no probed frame beneath, not attributed upward.
        e += caj_prof_anno_int(extra + e, "outside_selection_calls",
                               __cajeta_prof_instr_method_outside_calls(i));

        CajPbBuf b = { w->scratch, CAJ_PROF_SCRATCH, 0, 0 };
        __cajeta_prof_emit_slice_anno(&b, w->seq_id, ts, CAJ_INSTR_TRACK_UUID,
                                      CAJ_TE_INSTANT, iid, NULL, src, 0, 0,
                                      extra, e);
        caj_prof_flush(w, &b);
    }
    return __cajeta_prof_trace_packets(w) - before;
}

// The run record: an INSTANT on its own track carrying the run's configuration
// and its losses - host and GPU drop counts, calibration, instrumentation - as
// debug annotations. Emitted first, so it survives a truncated trace.
int32_t __cajeta_prof_trace_metadata(CajProfWriter* w, uint64_t ts,
                                     const char* tier, int32_t rate_hz,
                                     int32_t ring_cap, int64_t samples,
                                     int64_t dropped, int64_t frames) {
    if (!w || !w->f) return 0;
    const uint64_t meta_uuid = 0x1;
    __cajeta_prof_trace_track(w, meta_uuid, 0, "cajeta.profiler");

    uint8_t te[1792];
    int32_t n = __cajeta_pb_uint64(te, CAJ_PB_TE_TYPE, CAJ_TE_INSTANT);
    n += __cajeta_pb_uint64(te + n, CAJ_PB_TE_TRACK_UUID, meta_uuid);
    const char* label = "cajeta.profiler.run";
    int32_t ll = 0; while (label[ll]) ll++;
    n += __cajeta_pb_bytes(te + n, CAJ_PB_TE_NAME, (const uint8_t*) label, ll);
    n += caj_prof_anno_str(te + n, "tier", tier ? tier : "sampling");
    n += caj_prof_anno_int(te + n, "rate_hz", rate_hz);
    n += caj_prof_anno_int(te + n, "ring_capacity", ring_cap);
    n += caj_prof_anno_int(te + n, "samples_taken", samples);
    n += caj_prof_anno_int(te + n, "samples_dropped", dropped);
    n += caj_prof_anno_int(te + n, "frames_captured", frames);
    // A rate, so a reader need not divide; per mille, since the wire has no float.
    int64_t total = samples + dropped;
    n += caj_prof_anno_int(te + n, "dropped_per_mille",
                           total > 0 ? (dropped * 1000) / total : 0);
#ifndef CAJETA_PROF_TRACE_STANDALONE
    // The DEVICE side of the same question - samples_dropped covers the host
    // sampler only. This is the CAPTURE ring (what CAJETA_PROFILER_GPU_RING
    // sizes), NOT the per-sink queue; it OVERWRITES, so the oldest are lost.
    {
        int64_t gpu_dropped = __cajeta_prof_gpu_capture_dropped();
        int64_t gpu_kept    = __cajeta_prof_gpu_captured();
        int64_t gpu_total   = gpu_kept + gpu_dropped;
        n += caj_prof_anno_int(te + n, "gpu_records_dropped", gpu_dropped);
        n += caj_prof_anno_int(te + n, "gpu_records_kept", gpu_kept);
        n += caj_prof_anno_int(te + n, "gpu_dropped_per_mille",
                               gpu_total > 0 ? (gpu_dropped * 1000) / gpu_total : 0);
    }
#endif
    n += caj_prof_calibration_annos(te + n, (int32_t) sizeof(te) - n);
    n += caj_prof_instr_annos(te + n, (int32_t) sizeof(te) - n);
#ifndef CAJETA_PROF_TRACE_STANDALONE
    n += caj_prof_rocm_annos(te + n, (int32_t) sizeof(te) - n);
#endif
    uint8_t pkt[2048];
    int32_t p = __cajeta_pb_uint64(pkt, CAJ_PB_PKT_TIMESTAMP, ts);
    p += __cajeta_pb_uint64(pkt + p, CAJ_PB_PKT_SEQ_ID, w->seq_id);
    p += __cajeta_pb_bytes(pkt + p, CAJ_PB_PKT_TRACK_EVENT, te, n);
    CajPbBuf b = { w->scratch, CAJ_PROF_SCRATCH, 0, 0 };
    caj_pb_packet(&b, pkt, p);
    return caj_prof_flush(w, &b);
}

// Above is free of the sampler's globals; the drain below reads the ring.
#ifndef CAJETA_PROF_TRACE_STANDALONE

// Drains the sampler ring into `path`, consuming tail..head. The ring is contiguous
// only modulo capacity, so it is copied into order first, off the sampling path.
int64_t __cajeta_prof_drain_to_trace(const char* path) {
    if (!__cajeta_prof_ring || __cajeta_prof_ring_cap <= 0) return 0;
    int64_t head = __atomic_load_n(&__cajeta_prof_head, __ATOMIC_ACQUIRE);
    int64_t tail = __cajeta_prof_tail;
    int64_t n = head - tail;
    if (n <= 0) return 0;
    if (n > __cajeta_prof_ring_cap) n = __cajeta_prof_ring_cap;
    CajetaProfSample* ordered =
        (CajetaProfSample*) malloc((size_t) n * sizeof(CajetaProfSample));
    if (!ordered) return 0;
    for (int64_t i = 0; i < n; i++)
        ordered[i] = __cajeta_prof_ring[(tail + i) % __cajeta_prof_ring_cap];
    // The drain is the only place that sees both the ring and the counters.
    CajProfMeta meta;
    meta.tier = "sampling";
    meta.rate_hz = __cajeta_prof_interval > 0 ? 1000000 / __cajeta_prof_interval : 0;
    meta.ring_cap = __cajeta_prof_ring_cap;
    meta.samples = __cajeta_prof_samples;
    meta.dropped = __cajeta_prof_drops;
    meta.frames = __cajeta_prof_frames;
    int64_t packets = __cajeta_prof_samples_to_trace_meta(ordered, n, path, &meta);
    free(ordered);
    __cajeta_prof_tail = head;
    return packets;
}


// ── 4.2.d: drain-and-flush on normal exit ─────────────────────────────────
// Drain and flush on normal exit. Idempotent: main's epilogue, System.exit and
// tests all reach it, and two of those can happen in one run.
int32_t __cajeta_prof_gpu_trace_detach(void);   // cajeta_rt_prof_gpu.c, later in this TU
int64_t __cajeta_prof_gpu_only_to_trace(const char* path);   // ditto

static volatile int __cajeta_prof_shutdown_done = 0;

int64_t __cajeta_prof_shutdown(void) {
    if (__atomic_exchange_n(&__cajeta_prof_shutdown_done, 1, __ATOMIC_ACQ_REL))
        return 0;
    // Stop the sampler BEFORE reading the ring, or the copy loop races head and
    // the transform sees a torn sample.
    __cajeta_prof_disarm();
    __cajeta_prof_gpu_trace_detach();           // flush any attached GPU trace
    int64_t packets = __cajeta_prof_drain_to_trace(__cajeta_prof_out_path());
    // Nothing sampled, but an instrumented build still has exact counts.
    if (packets == 0)
        packets = __cajeta_prof_instr_only_to_trace(__cajeta_prof_out_path());
    // A run that dispatched to the GPU but sampled nothing still measured work.
    if (packets == 0)
        packets = __cajeta_prof_gpu_only_to_trace(__cajeta_prof_out_path());
    return packets;
}

// Tests arm and drain repeatedly in one process, so shutdown must be re-armable.
void __cajeta_prof_shutdown_reset(void) { __cajeta_prof_shutdown_done = 0; }

#endif  /* CAJETA_PROF_TRACE_STANDALONE */
