// cajeta-profiler Unit 5 — Perfetto trace writer.
//
// Perfetto protobuf, emitted directly. No SDK, no third-party build dependency
// (5.3.b): a .pftrace file is just a sequence of length-delimited TracePacket
// messages, each written as field 1 of the Trace message. There is no header and
// no footer, which is precisely what makes spec §7.6 achievable — a trace
// truncated mid-write stays readable up to the truncation, because every packet
// carries its own length and nothing at the end is required to interpret what
// came before.
//
// Chrome Trace Event JSON was rejected for this (spec §14.3): its clock-sync
// events are not parsed by any current consumer, so every timestamp would have
// to be pre-converted into one domain at emit time — exactly what §6 shows we
// cannot correctly do.
//
// This file holds the wire primitives. They are pure, deterministic, and
// verified by round-trip; the schema mapping (which field number carries what)
// is a separate concern and is called out where it begins.

// ── protobuf wire primitives ─────────────────────────────────────────────
// Wire types: 0 varint, 2 length-delimited. Those are the only two needed —
// every field this writer emits is an integer, a string, or a submessage.
#define CAJ_PB_WIRE_VARINT 0
#define CAJ_PB_WIRE_FIXED64 1
#define CAJ_PB_WIRE_BYTES  2

// Base-128 varint, little-endian groups, high bit = continuation. Returns the
// number of bytes written (1..10). `out` must have room for 10.
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

// Inverse. Reads at most `max` bytes; sets *consumed. Returns the value, or 0
// with *consumed = -1 if the encoding runs past `max` or exceeds 10 bytes —
// a truncated trace ends in exactly this condition and must be detected, not
// read as a valid small number.
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

// field: length-delimited payload (string or submessage — identical on the
// wire, which is why a submessage can be built in a scratch buffer and then
// appended without re-encoding).
// A fixed64 field (wire type 1): eight little-endian bytes, no varint. Needed
// because TrackEvent.flow_ids is `repeated fixed64` — writing it as a varint
// yields a packet that parses and a flow that never appears.
int32_t __cajeta_pb_fixed64(uint8_t* out, uint32_t field, uint64_t v) {
    int32_t n = __cajeta_pb_tag(out, field, CAJ_PB_WIRE_FIXED64);
    for (int32_t i = 0; i < 8; i++) out[n + i] = (uint8_t) ((v >> (i * 8)) & 0xFF);
    return n + 8;
}

int32_t __cajeta_pb_bytes(uint8_t* out, uint32_t field,
                          const uint8_t* data, int32_t len) {
    int32_t n = __cajeta_pb_tag(out, field, CAJ_PB_WIRE_BYTES);
    n += __cajeta_pb_varint(out + n, (uint64_t) len);
    for (int32_t i = 0; i < len; i++) out[n + i] = data[i];
    return n + len;
}

// ── trace assembly ────────────────────────────────────────────────────────
// Field numbers below are VERIFIED against third_party/perfetto/perfetto_trace.proto
// (see its PROVENANCE.md for the table and the sha256 they were read from). They
// are not recalled. A wrong number here yields a file that is valid protobuf and
// still does not load — every local test passes and the artifact is useless —
// which is why the proto is vendored and why CI runs trace_processor over the
// output rather than trusting this comment.
//
// Spec §7.7 confines the writer to the stable TrackEvent / TrackDescriptor
// schema. Anything GPU-specific is an optional addition, because those parts are
// explicitly outside Perfetto's stability guarantee.
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

// TracePacket.SequenceFlags. The two are a PAIR and the ordering is not
// cosmetic — established by CI (run 32489238054), which loaded a trace whose
// every slice name came back [NULL].
//
//   CLEARED  goes on the packet that ESTABLISHES incremental state, i.e. the
//            first InternedData of a sequence. It means "nothing before this
//            point applies". Putting it on the first slice instead — as this
//            file originally did — tells the reader to discard the interned
//            names emitted just before it.
//   NEEDS    goes on every packet that CONSUMES incremental state. The proto is
//            explicit that a reader SKIPS such a packet when no CLEARED has been
//            seen on the sequence, so a slice referencing name_iid without this
//            flag silently loses its name rather than failing.
//
// Both are wire-valid and field-number-correct in either arrangement, which is
// exactly why only a real reader could catch this.
#define CAJ_PB_SEQ_FLAG_CLEARED 1
#define CAJ_PB_SEQ_FLAG_NEEDS   2

typedef struct {
    uint8_t* buf;
    int32_t  cap;
    int32_t  len;
    int32_t  overflow;   // sticky: a caller that ignores it cannot get a
                         // silently-short trace back
} CajPbBuf;

static int32_t caj_pb_put(CajPbBuf* b, const uint8_t* d, int32_t n) {
    if (b->len + n > b->cap) { b->overflow = 1; return 0; }
    for (int32_t i = 0; i < n; i++) b->buf[b->len + i] = d[i];
    b->len += n;
    return n;
}

// Wrap a built payload as one Trace.packet. This framing is the whole reason a
// truncated trace stays readable (§7.6): every packet carries its own length and
// nothing at the end of the file is needed to interpret what came before.
static int32_t caj_pb_packet(CajPbBuf* out, const uint8_t* payload, int32_t len) {
    uint8_t hdr[16];
    int32_t n = __cajeta_pb_tag(hdr, CAJ_PB_TRACE_PACKET, CAJ_PB_WIRE_BYTES);
    n += __cajeta_pb_varint(hdr + n, (uint64_t) len);
    if (!caj_pb_put(out, hdr, n)) return 0;
    return caj_pb_put(out, payload, len) ? n + len : 0;
}

// TrackDescriptor: names a track and optionally parents it, which is what makes
// device/context/queue a real hierarchy rather than synthetic threads (§7.2).
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

// InternedData carrying one EventName. §7.4: a repeated name is emitted once and
// referenced by iid thereafter.
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

// One TrackEvent. `name_iid` references an interned name; pass 0 with a literal
// `name` only for one-off events.
// `extra` is pre-encoded TrackEvent bytes appended verbatim — today the
// debug annotations §10.6 wants on every device measurement. Passed as bytes
// rather than as a struct so the annotation vocabulary can grow without this
// signature moving again; NULL for the common case.
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
    // A flow needs BOTH ends: the launch site lists the id, the device slice
    // terminates it. Listing an id on only one event draws no arrow at all,
    // and the trace still loads (spec §7.3).
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
    // A slice that references an interned name CONSUMES incremental state.
    // Without this the reader skips the association and the slice loads with a
    // null name — the trace still parses, which is the trap.
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

// The flow-free form every non-GPU caller uses. One implementation, so a fix to
// the sequence flags cannot land in only one of them.
int32_t __cajeta_prof_emit_slice(CajPbBuf* out, uint32_t seq_id, uint64_t ts,
                                 uint64_t track_uuid, int32_t type,
                                 uint64_t name_iid, const char* name,
                                 uint64_t source_iid) {
    return __cajeta_prof_emit_slice_flow(out, seq_id, ts, track_uuid, type,
                                         name_iid, name, source_iid, 0, 0);
}

// ── streaming writer + interning table (5.2.c, 5.2.d) ─────────────────────
// Packets are appended to the file as they are built, never accumulated. A
// profiled run can be killed at any moment (§7.6), and a writer that buffered
// the whole trace would lose everything it had — the opposite of the property
// the packet framing exists to provide. One scratch buffer is reused per packet
// so the writer allocates nothing after open.
#define CAJ_PROF_MAX_INTERNED 4096
#define CAJ_PROF_SCRATCH 4096
#define CAJ_PROF_NAME_POOL (256 * 1024)

typedef struct {
    FILE*       f;
    uint32_t    seq_id;
    int32_t     n_names;
    // The table OWNS its names. It used to keep the caller's pointer, which is
    // a dangling reference the moment a caller passes a scratch buffer — and
    // Unit 6's transform does exactly that, building "Type.method" into a local
    // before each slice. The failure was silent and total: on the next call the
    // reused buffer held the NEXT name, strcmp matched it, and every frame
    // resolved to the first iid. CI run 32491747115 caught it as a trace
    // containing exactly one distinct name.
    int32_t     name_off[CAJ_PROF_MAX_INTERNED];
    // Source locations intern in their OWN iid space (Perfetto keys interning
    // per field, not per sequence). Keeping them separate is what preserves
    // §7.4: a method sampled at forty lines is still ONE EventName, with forty
    // SourceLocations beside it. Folding "file:line" into the slice name would
    // have minted a fresh interned name per line and defeated the whole table.
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

// Returns the iid for `name`, emitting an InternedData packet the first time it
// is seen (§7.4: a repeated name is emitted once). iids are 1-based; 0 means
// "not interned", which is what TrackEvent.name_iid treats as absent.
//
// Linear scan with strcmp rather than pointer identity: the frame descriptors
// codegen emits are per-method, so two methods of the same name in different
// modules carry equal strings at different addresses. Pointer identity would
// silently emit the same name repeatedly and defeat the interning.
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

// Copy a string into the writer's pool, returning its offset or -1. Shared by
// both interning tables, and the reason neither can be left holding a caller's
// scratch buffer.
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
    // caj_pb_packet, NOT caj_pb_put: the payload must be wrapped as
    // Trace.packet. Writing it raw put unframed bytes in the file, and the
    // slices then referenced source-location iids that were never emitted. The
    // trace still LOADED — trace_processor skipped what it could not parse and
    // reported source_location_iid as an unresolved arg — so nothing failed,
    // the locations were simply absent.
    caj_pb_packet(&b, pkt, p);
    return caj_prof_flush(w, &b);
}

// Intern a (file, function, line) triple. Returns its iid, or 0 if it could not
// be interned — in which case the caller omits the field rather than pointing a
// slice at the wrong line.
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

int32_t __cajeta_prof_trace_track(CajProfWriter* w, uint64_t uuid,
                                  uint64_t parent_uuid, const char* name) {
    if (!w || !w->f) return 0;
    CajPbBuf b = { w->scratch, CAJ_PROF_SCRATCH, 0, 0 };
    __cajeta_prof_emit_track(&b, uuid, parent_uuid, name);
    return caj_prof_flush(w, &b);
}

// Interns `name` and emits the slice referencing it. A slice with no name (a
// SLICE_END) passes name = NULL and carries neither field.
// `file` and `line` are optional: pass NULL / 0 for a slice with no source
// position (a SLICE_END, which needs neither).
//
// The line recorded is the one observed WHEN THE SLICE OPENED. A frame stays on
// the stack across many samples at many lines; the slice says where it was first
// seen, not where it spent its time. Reading it as the latter would be wrong,
// which is why it is a source_location and not a duration attribution.
int32_t __cajeta_prof_trace_slice_at(CajProfWriter* w, uint64_t ts,
                                     uint64_t track_uuid, int32_t type,
                                     const char* name, const char* file,
                                     int32_t line) {
    if (!w || !w->f) return 0;
    uint64_t iid = name ? __cajeta_prof_intern(w, name) : 0;
    uint64_t src = (file && name) ? __cajeta_prof_intern_source(w, file, name, line) : 0;
    CajPbBuf b = { w->scratch, CAJ_PROF_SCRATCH, 0, 0 };
    // iid == 0 with a non-NULL name means the table was full or the intern
    // failed; fall back to the inline name rather than dropping it.
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

int32_t __cajeta_prof_trace_close(CajProfWriter* w) {
    if (!w || !w->f) return 0;
    // No footer: there is nothing to write at the end, which is the same
    // property that makes a killed run's partial trace readable.
    int r = fflush(w->f) == 0;
    fclose(w->f);
    w->f = NULL;
    return r;
}

// ── Unit 6: samples become slices ─────────────────────────────────────────
// A sampler produces periodic STACKS; Perfetto's TrackEvent model wants
// SLICES. The conversion is a per-track diff against the previously open stack:
// frames still present are left open, frames that vanished are closed innermost
// first, frames that appeared are opened outermost first. Everything still open
// is closed at drain.
//
// WHAT THIS COSTS, stated because a flame graph invites the opposite reading:
// every slice boundary lands on a SAMPLE TICK, not on a real call boundary. A
// slice says "this frame was on the stack at these ticks", never "this call
// started here". At the 1 kHz default a call shorter than a millisecond may not
// appear at all, and one that does appear has its edges quantized to the
// interval. That is inherent to sampling — §3's instrumentation tier exists for
// exact enter/exit — and §7.8 requires the trace record which tier produced a
// measurement so a reader can tell the two apart.
//
// The diff is also why interning matters: a frame that persists across a
// thousand ticks is named once, not a thousand times.
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

// Stable, readable track names. A fiber gets its debugger id, which is the same
// id the DAP fibers view shows, so a profile and a debug session name the same
// fiber the same way.
static void caj_prof_track_name(char* out, int32_t cap, const CajProfTrack* t,
                                int32_t index) {
    const char* kind = (t->kind == CAJETA_PROF_OWNER_FIBER) ? "fiber" : "thread";
    // A fiber's id comes from the SAMPLE, not from the handle. Reading it here
    // meant dereferencing a fiber that a drain-at-exit finds long dead; the
    // handle is only known live at the moment it was sampled.
    long id = (t->kind == CAJETA_PROF_OWNER_FIBER) ? (long) t->id : (long) index;
    snprintf(out, (size_t) cap, "cajeta.%s.%ld", kind, id);
}

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
    // uuid must be stable and non-zero; the handle's address is both, and it is
    // never dereferenced here.
    t->uuid = (uint64_t) (uintptr_t) owner;
    char name[64];
    caj_prof_track_name(name, (int32_t) sizeof(name), t, *n);
    __cajeta_prof_trace_track(w, t->uuid, 0, name);
    (*n)++;
    return t;
}

// One frame's display name: "Type.method". Built into a caller buffer so the
// interning table sees a stable content string.
static void caj_prof_frame_name(char* out, int32_t cap, const CajetaFrameDesc* d) {
    const char* t = (d && d->typeName) ? d->typeName : "?";
    const char* m = (d && d->methodName) ? d->methodName : "?";
    snprintf(out, (size_t) cap, "%s.%s", t, m);
}

// Convert an ordered run of samples into a trace at `path`. Returns packets
// written, or 0. The `_meta` form additionally stamps §7.8's run metadata.
//
// Takes the samples as a parameter rather than reading the ring, so the exact
// code CI validates is the code the runtime runs: tools/tracegen builds
// synthetic samples and calls this under a plain `cc`. A transform that reached
// into globals could only ever be tested by building the whole toolchain, which
// is expensive enough that it would not be tested on every change.
// Defined below, next to the annotation helpers it uses.
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

int64_t __cajeta_prof_samples_to_trace_meta(const CajetaProfSample* samples,
                                            int64_t n, const char* path,
                                            const CajProfMeta* meta) {
    if (!samples || n <= 0) return 0;
    static CajProfWriter w;
    if (!__cajeta_prof_trace_open(&w, path)) return 0;
    // Settle the GPU side FIRST: the metadata packet below carries the ROCm
    // backend's account of itself (§5.2.2), and asking before the last records
    // are claimed reports rocm_records=0 next to a file full of device spans.
    __cajeta_prof_gpu_capture_settle();
    // First packet, so it survives a trace truncated moments later.
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

        // Common prefix with what is already open.
        int32_t common = 0;
        while (common < n && common < t->depth && cur[common] == t->open[common])
            common++;

        // Close what vanished, innermost first — Perfetto requires SLICE_END in
        // reverse order of SLICE_BEGIN on a track.
        for (int32_t k = t->depth - 1; k >= common; k--)
            __cajeta_prof_trace_slice(&w, (uint64_t) s->host_ns, t->uuid,
                                      CAJ_TE_SLICE_END, NULL);
        // Open what appeared, outermost first.
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

    // Close every slice still open, or the trace ends with unterminated slices
    // and a reader has to guess where they stopped.
    for (int32_t i = 0; i < n_tracks; i++)
        for (int32_t k = tracks[i].depth - 1; k >= 0; k--)
            __cajeta_prof_trace_slice(&w, (uint64_t) last_ts, tracks[i].uuid,
                                      CAJ_TE_SLICE_END, NULL);

    // §3.4 — if this build also carries instrumentation probes, its per-method
    // records go in the SAME trace, on their own track. Two files would make
    // "which tier produced this number" a question about provenance the reader
    // has to keep track of by hand.
    __cajeta_prof_instr_to_trace(&w, (uint64_t) last_ts);

    // 6.6 — and the GPU work this run captured, on its own device/context/queue
    // tracks, in the SAME file for the same reason (§8.3's one time axis).
    // Before this, an env-armed run of a GPU program wrote a trace with no
    // device track at all.
    __cajeta_prof_gpu_captured_to_trace(&w, (uint64_t) last_ts);

    int64_t packets = __cajeta_prof_trace_packets(&w);
    __cajeta_prof_trace_close(&w);
    return packets;
}

// An INSTRUMENTED run with no sampler still has a profile to write, and until
// this existed it wrote nothing: the drain returns early on an empty ring, so
// `--profiler=instrument` without CAJETA_PROFILER produced exact counts that
// never left memory.
int64_t __cajeta_prof_instr_only_to_trace(const char* path) {
    if (!__cajeta_prof_instr_is_present()) return 0;
    if (__cajeta_prof_instr_method_count() <= 0) return 0;
    static CajProfWriter w;
    if (!__cajeta_prof_trace_open(&w, path)) return 0;
    // Timestamp 0: the counters are totals over the run, not events at a
    // moment, and stamping them with "now" would put them after work they
    // summarize. The run record carries the same stamp for the same reason.
    __cajeta_prof_trace_metadata(&w, 0, "instrumentation", 0, 0, 0, 0, 0);
    __cajeta_prof_instr_to_trace(&w, 0);
    int64_t packets = __cajeta_prof_trace_packets(&w);
    __cajeta_prof_trace_close(&w);
    return packets;
}

// ── GPU dispatch records -> trace (Unit 7; spec §7.2, §7.3) ───────────────
//
// Lives here, beside the sample transform and on the same side of
// CAJETA_PROF_TRACE_STANDALONE, for the reason the sample transform does: this
// is what tools/tracegen drives under a plain `cc`, so CI's trace_processor
// judges the emitter that ships. A GPU transform that could only run with a GPU
// attached would be checked by nobody.

// Track uuids. The top nibble is set so a synthetic track can never collide
// with a HOST track, whose uuid is a thread handle's address — no pointer on
// any supported platform reaches bit 60.
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

// Mirrors the backend enum in cajeta_xpu_dispatch.c, which is included LATER in
// the single-TU build and so cannot be referenced from here. Presentation only:
// a wrong name here mislabels a track and breaks nothing else.
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

// Which track descriptors this writer has already emitted. Held by the caller
// rather than inside CajProfWriter so the streaming sink can emit across many
// batches without re-declaring a track it already named.
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

// Emit `n` dispatch records into an open writer. Returns packets written.
// Defined with the metadata writer below; declared here so the GPU emitter can
// annotate a device slice with the tier and confidence §10.6 requires.
static int32_t caj_prof_anno_int(uint8_t* out, const char* name, int64_t v);

// §7.5 — one ClockSnapshot pairing the host clock with a device domain, so a
// reader can reproduce the mapping instead of taking the converted timestamps
// on trust. The device clock id is sequence-scoped (Perfetto reserves [64,127]
// for user clocks and scopes them to the emitting sequence), which is why the
// writer keeps one sequence id for the whole file.
//
// unit_multiplier_ns is 1: the device timestamp is recorded here in TICKS,
// with the tick period carried by the correlation, because a snapshot that
// pre-converted its own device value would be describing the conversion using
// the conversion.
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

int64_t __cajeta_prof_gpu_emit(CajProfWriter* w, CajGpuTracks* seen,
                               const CajetaGpuEvent* evs, int32_t n) {
    if (!w || !seen || !evs || n <= 0) return 0;
    int64_t before = __cajeta_prof_trace_packets(w);

    // §7.5 — every calibration this run performed, replayed into the trace
    // before the spans that depend on it. Emitted here rather than at
    // calibration time because the writer runs at drain, and a snapshot that
    // arrived after the spans it explains would be useless to a streaming
    // reader.
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

        // device -> context -> queue, a real hierarchy (§7.2). A flat set of
        // three roots would render identically in a one-device trace and wrongly
        // the moment there are two.
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
        // The launching thread's track. Named from the handle rather than from
        // the sampler's registry index, which this transform cannot see — when
        // Unit 9 merges the sampled and dispatched halves into one trace the two
        // namings must be reconciled; the UUID (the handle address) already
        // agrees, so the merge is a naming fix, not a re-identification.
        uint64_t host = (uint64_t) (uintptr_t) e->host_thread;
        if (host && !caj_gpu_track_seen(seen, host)) {
            snprintf(nm, sizeof(nm), "cajeta.thread.%llu", (unsigned long long) host);
            __cajeta_prof_trace_track(w, host, 0, nm);
        }

        const char* kn = e->kernel_name ? e->kernel_name : "?";
        // Host launch site: an instant on the launching thread, carrying the
        // flow id and the source location, so "which line launched this" is one
        // click from the device slice (§5.1.2, §7.3).
        if (host) {
            const CajetaFrameDesc* d = e->call_site;
            uint64_t iid = __cajeta_prof_intern(w, kn);
            // "Type.method", via the SAME helper the sampler uses. Interning
            // the bare method name here would give one call site two different
            // names depending on which half of the profiler saw it — `sum` in
            // the launch flow and `gpu.Reduce.sum` in the sampled stack — and a
            // reader correlating the two would find no match and have no way to
            // tell that from the launch genuinely not being sampled.
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
        // Device execution: a slice on the queue track, terminating the flow.
        //
        // §10.6 — the tier and the correlation confidence ride on the
        // measurement itself, not on a run-level note, because a single run can
        // mix them: one backend demoted, another not. A developer must never
        // have to infer that a span in front of them was degraded. §11.3's
        // integrity flags ride along for the same reason — a flagged span still
        // renders, and only the annotation says it should not be trusted.
        {
            uint64_t iid = __cajeta_prof_intern(w, kn);
            uint8_t anno[256];
            int32_t a = caj_prof_anno_int(anno, "tier", e->tier);
            a += caj_prof_anno_int(anno + a, "clock_confidence",
                                   __cajeta_prof_clock_confidence(e->backend));
            // What the checker derives, OR'd with what only the producer could
            // know (a Vulkan timestamp-register reset is visible solely in the
            // backend's own span history — §5.5.7).
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
    // §7.8 applies to a device trace exactly as it does to a sampled one, and
    // this is the run record's only home on this path. The sampler counters are
    // zero because no sampling happened — which is true, and better than
    // borrowing the event count to fill a field that means something else.
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
// An INSTANT event on its own track, carrying the run's configuration and its
// losses as debug annotations. Emitted first so it is present even in a trace
// truncated seconds later.
//
// The drop count is the reason this exists. The sampler drops on ring overflow
// rather than blocking — the right call, since blocking would perturb the
// program it measures — but until now that number lived only in memory. A trace
// that lost a third of its samples was byte-for-byte indistinguishable from one
// that lost none, so a flame graph built from a starved ring looked exactly as
// authoritative as a complete one. §7.8 requires the trace state which tier
// produced each measurement, and "how much did we miss" is the same question.
//
// DebugAnnotation rather than a bespoke packet: it lives on TrackEvent, which
// §7.7 confines us to, and Perfetto surfaces it in the UI as arguments on the
// event.
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

// §7.8 — per-domain calibration quality, plus the driver identity and active
// layers the owning backend registered. Emitted only for domains that actually
// calibrated: an uncalibrated run must not invent a quality figure, because
// "there wasn't one" and "it was poor" call for different responses.
//
// Drift is integer MILLI-ppm. The wire carries no floats, and rounding to whole
// ppm would render the reference device's −15 ppm (§6.6) as −15 while a −0.4 ppm
// device became 0 — erasing precisely the term that unit is about.
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
        // Six ints and two strings per domain; stop before the buffer rather
        // than truncate an annotation mid-field, which would corrupt the packet
        // instead of shortening it.
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

// §3.5 / §3.12 / §3.13 — what an INSTRUMENTED build cost, what it left out,
// and what it was optimized at. Emitted only when instrumentation probes
// actually exist: a sampled run must not carry a zero-valued instrumentation
// record, because "no probes were built" and "the probes recorded nothing" are
// different facts and only one of them means the developer should look again.
//
// The overhead is pairs x a CALIBRATED per-pair cost, not a constant. §3.5
// exists so the developer can judge how much the measurement distorted the
// program, and a figure this file guessed at would read exactly like a measured
// one.
#ifndef CAJETA_PROF_TRACE_STANDALONE
// ── Unit 8 — what the ROCm backend actually did (§5.2, §6.4) ─────────────
//
// Present on every trace from a run that attempted the ROCm backend, including
// — especially — the runs where it did not work. A degraded trace that looks
// identical to a device-timed one is the failure §5.2.2 is about, and the state
// plus the reason are what let a reader tell them apart without being there.
static int32_t caj_prof_rocm_annos(uint8_t* out, int32_t cap) {
    int32_t n = 0;
    const int32_t state = __cajeta_prof_rocm_state();
    if (state == CAJETA_ROCM_UNATTEMPTED) return 0;   // no GPU run; say nothing
    if (cap < 640) return 0;

    n += caj_prof_anno_int(out + n, "rocm_state", state);
    n += caj_prof_anno_int(out + n, "rocm_tracing", __cajeta_prof_rocm_tracing());
    n += caj_prof_anno_int(out + n, "rocm_launches", __cajeta_prof_rocm_launches());
    n += caj_prof_anno_int(out + n, "rocm_records", __cajeta_prof_rocm_records());
    // Records that matched no launch of ours — HIP's own fill and copy kernels.
    // Reported rather than hidden: a reader comparing launches to records would
    // otherwise conclude the correlation was leaking.
    n += caj_prof_anno_int(out + n, "rocm_unmatched_records",
                           __cajeta_prof_rocm_unmatched());
    n += caj_prof_anno_int(out + n, "rocm_clock_offset_ns",
                           __cajeta_prof_rocm_clock_offset_ns());
    // §6.4 — a trace that spans a suspend has everything after the sleep sitting
    // minutes out of place while rendering perfectly, so the file has to say so.
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
    // §3.13 — the flag pins no optimization level, so the level is part of what
    // every number here means and is never left for the reader to infer.
    n += caj_prof_anno_int(out + n, "instr_opt_level",
                           __cajeta_prof_instr_opt_level());
    // §3.12 — a profile that silently omits code reads as though that code were
    // free, so the selection travels with the run.
    const char* sel = __cajeta_prof_instr_selection();
    n += caj_prof_anno_str(out + n, "instr_selection",
                           (sel && sel[0]) ? sel : "all");
    return n;
}

// §3.4 — the per-method records, on a track of their own. Sampling lands as
// nested SLICE_BEGIN/END on per-thread tracks; instrumentation lands here as
// one INSTANT per method, and each carries `source` explicitly as well. Both
// live in one trace and a consumer can always say which produced a number,
// which is the whole requirement — a merged timeline whose provenance is a
// guess is worse than two separate files.
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
        // §3.11 — entries reached with no probed frame beneath them. Recorded
        // as a fact about this method rather than attributed to the nearest
        // probed ancestor, which would be a fabricated call edge.
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
    // Stated as a rate so a reader does not have to divide to learn whether the
    // profile is trustworthy. Parts per thousand: integer, no float on the wire.
    int64_t total = samples + dropped;
    n += caj_prof_anno_int(te + n, "dropped_per_mille",
                           total > 0 ? (dropped * 1000) / total : 0);
#ifndef CAJETA_PROF_TRACE_STANDALONE
    // The DEVICE side of the same question. samples_dropped covers the host
    // sampler only, so the guide's "check the drop count first" had no answer
    // for a GPU ring that overflowed: a run keeping 8192 of 56,843 launches
    // produced the same record as one that kept every launch (Julian,
    // 2026-09-02).
    //
    // The CAPTURE ring, which is the one CAJETA_PROFILER_GPU_RING sizes and the
    // one that overflows under a busy kernel loop. NOT the per-sink queue: its
    // counters look adjacent and are a different ring entirely, so reporting
    // them here would read zero while launches were being lost — worse than
    // saying nothing. Measured while writing this: capture ring 8 vs sink
    // counters, which stayed at 0.
    //
    // The ring OVERWRITES, so what survives is the most recent `capacity`
    // records and what is lost is the oldest.
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
    // §3.5/§3.12/§3.13 — present only on a build that actually carries probes.
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

// Everything above is free of the sampler's globals, so tools/tracegen can
// compile it standalone and CI can validate the real transform. The ring drain
// below is the one part that cannot be — it reads the sampler's ring — so it is
// excluded from that build rather than duplicated for it.
#ifndef CAJETA_PROF_TRACE_STANDALONE

// Drain the sampler ring into `path`, consuming tail..head. This is the drain
// half of 4.2.d: a run that ends normally flushes what it holds, and one that is
// killed leaves a shorter but still readable trace (§7.6).
//
// The ring is contiguous only modulo its capacity, so it is copied into order
// before conversion. That copy is at drain time, off the sampling path.
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
    // The drain is the only place that can see BOTH the ring's contents and the
    // sampler's counters, so it is where §7.8's metadata is stamped.
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
//
// Until this existed, §9.1's promise — "when CAJETA_PROFILER is set, the run is
// profiled" — was only half true: the sampler filled the ring and the program
// exited without writing it. A profiled run produced nothing unless the caller
// knew to drain by hand, which no user of a default-built binary does.
//
// Idempotent, and deliberately so: it is reached from main's epilogue, from
// System.exit, and from tests, and two of those can happen in one run. A second
// call must not truncate the file the first one wrote.
int32_t __cajeta_prof_gpu_trace_detach(void);   // cajeta_rt_prof_gpu.c, later in this TU
int64_t __cajeta_prof_gpu_only_to_trace(const char* path);   // ditto

static volatile int __cajeta_prof_shutdown_done = 0;

int64_t __cajeta_prof_shutdown(void) {
    if (__atomic_exchange_n(&__cajeta_prof_shutdown_done, 1, __ATOMIC_ACQ_REL))
        return 0;
    // Stop the sampler BEFORE reading the ring. Draining under a live producer
    // races head against the copy loop and hands the transform a torn sample —
    // the kind of corruption that shows up as one impossible stack in a thousand
    // and gets blamed on the program under test.
    __cajeta_prof_disarm();
    __cajeta_prof_gpu_trace_detach();           // flush any attached GPU trace
    int64_t packets = __cajeta_prof_drain_to_trace(__cajeta_prof_out_path());
    // Nothing sampled — but an instrumented build still has exact counts, and
    // §3.1 promises them whether or not anyone armed the sampler.
    if (packets == 0)
        packets = __cajeta_prof_instr_only_to_trace(__cajeta_prof_out_path());
    // 6.6 — and a run that dispatched to the GPU but collected no samples still
    // measured something. The drain returns early on an empty ring, so without
    // this a short GPU program would profile to nothing.
    if (packets == 0)
        packets = __cajeta_prof_gpu_only_to_trace(__cajeta_prof_out_path());
    return packets;
}

// Tests arm and drain repeatedly in one process; without this the second run in
// a process would find shutdown already spent and write nothing.
void __cajeta_prof_shutdown_reset(void) { __cajeta_prof_shutdown_done = 0; }

#endif  /* CAJETA_PROF_TRACE_STANDALONE */
