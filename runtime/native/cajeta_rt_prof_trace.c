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
int32_t __cajeta_prof_emit_slice(CajPbBuf* out, uint32_t seq_id, uint64_t ts,
                                 uint64_t track_uuid, int32_t type,
                                 uint64_t name_iid, const char* name,
                                 uint64_t source_iid) {
    uint8_t te[512];
    int32_t n = __cajeta_pb_uint64(te, CAJ_PB_TE_TYPE, (uint64_t) type);
    n += __cajeta_pb_uint64(te + n, CAJ_PB_TE_TRACK_UUID, track_uuid);
    if (name_iid) {
        n += __cajeta_pb_uint64(te + n, CAJ_PB_TE_NAME_IID, name_iid);
    } else if (name) {
        int32_t ln = 0; while (name[ln] && ln < 400) ln++;
        n += __cajeta_pb_bytes(te + n, CAJ_PB_TE_NAME, (const uint8_t*) name, ln);
    }
    if (source_iid) n += __cajeta_pb_uint64(te + n, CAJ_PB_TE_SOURCE_LOC_IID, source_iid);
    uint8_t pkt[700];
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
    caj_pb_put(&b, pkt, p);
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
// Defined in cajeta_rt_concurrent_exec.c, later in this TU — same forward
// declaration pattern core.c uses for __cajeta_currentTimeNanos.
long __cajeta_dbg_fiber_id_of(void* fiber);

#define CAJ_PROF_MAX_TRACKS 256
#define CAJ_PROF_MAX_DEPTH  CAJETA_PROF_MAX_FRAMES

typedef struct {
    void*   owner;
    int32_t kind;
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
    long id = (t->kind == CAJETA_PROF_OWNER_FIBER)
                  ? __cajeta_dbg_fiber_id_of(t->owner)
                  : (long) index;
    snprintf(out, (size_t) cap, "cajeta.%s.%ld", kind, id);
}

static CajProfTrack* caj_prof_track_for(CajProfTrack* tracks, int32_t* n,
                                        CajProfWriter* w, void* owner,
                                        int32_t kind) {
    for (int32_t i = 0; i < *n; i++)
        if (tracks[i].owner == owner) return &tracks[i];
    if (*n >= CAJ_PROF_MAX_TRACKS) return NULL;
    CajProfTrack* t = &tracks[*n];
    t->owner = owner;
    t->kind = kind;
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
// written, or 0.
//
// Takes the samples as a parameter rather than reading the ring, so the exact
// code CI validates is the code the runtime runs: tools/tracegen builds
// synthetic samples and calls this under a plain `cc`. A transform that reached
// into globals could only ever be tested by building the whole toolchain, which
// is expensive enough that it would not be tested on every change.
int64_t __cajeta_prof_samples_to_trace(const CajetaProfSample* samples,
                                       int64_t n, const char* path) {
    if (!samples || n <= 0) return 0;
    static CajProfWriter w;
    if (!__cajeta_prof_trace_open(&w, path)) return 0;

    static CajProfTrack tracks[CAJ_PROF_MAX_TRACKS];
    int32_t n_tracks = 0;
    int64_t last_ts = 0;
    for (int64_t i = 0; i < n; i++) {
        const CajetaProfSample* s = &samples[i];
        CajProfTrack* t = caj_prof_track_for(tracks, &n_tracks, &w,
                                             s->owner, s->owner_kind);
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

    int64_t packets = __cajeta_prof_trace_packets(&w);
    __cajeta_prof_trace_close(&w);
    return packets;
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
    int64_t packets = __cajeta_prof_samples_to_trace(ordered, n, path);
    free(ordered);
    __cajeta_prof_tail = head;
    return packets;
}

#endif  /* CAJETA_PROF_TRACE_STANDALONE */
