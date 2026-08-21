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
#define CAJ_PB_ID_EVENT_NAMES      2    /* InternedData.event_names            */
#define CAJ_PB_EN_IID              1    /* EventName.iid                       */
#define CAJ_PB_EN_NAME             2    /* EventName.name                      */

#define CAJ_TE_SLICE_BEGIN 1
#define CAJ_TE_SLICE_END   2
#define CAJ_TE_INSTANT     3

// SEQ_INCREMENTAL_STATE_CLEARED — tells a reader the interning table starts
// fresh here. Emitted on the first packet of a sequence so a trace that begins
// mid-stream, or one truncated and resumed, is still interpretable.
#define CAJ_PB_SEQ_FLAG_CLEARED 1

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
                                uint64_t iid, const char* name) {
    int32_t ln = 0; while (name && name[ln] && ln < 400) ln++;
    uint8_t en[512];
    int32_t e = __cajeta_pb_uint64(en, CAJ_PB_EN_IID, iid);
    e += __cajeta_pb_bytes(en + e, CAJ_PB_EN_NAME, (const uint8_t*) name, ln);
    uint8_t id[600];
    int32_t d = __cajeta_pb_bytes(id, CAJ_PB_ID_EVENT_NAMES, en, e);
    uint8_t pkt[700];
    int32_t p = __cajeta_pb_uint64(pkt, CAJ_PB_PKT_SEQ_ID, seq_id);
    p += __cajeta_pb_bytes(pkt + p, CAJ_PB_PKT_INTERNED, id, d);
    return caj_pb_packet(out, pkt, p);
}

// One TrackEvent. `name_iid` references an interned name; pass 0 with a literal
// `name` only for one-off events.
int32_t __cajeta_prof_emit_slice(CajPbBuf* out, uint32_t seq_id, uint64_t ts,
                                 uint64_t track_uuid, int32_t type,
                                 uint64_t name_iid, const char* name,
                                 int32_t first_in_sequence) {
    uint8_t te[512];
    int32_t n = __cajeta_pb_uint64(te, CAJ_PB_TE_TYPE, (uint64_t) type);
    n += __cajeta_pb_uint64(te + n, CAJ_PB_TE_TRACK_UUID, track_uuid);
    if (name_iid) {
        n += __cajeta_pb_uint64(te + n, CAJ_PB_TE_NAME_IID, name_iid);
    } else if (name) {
        int32_t ln = 0; while (name[ln] && ln < 400) ln++;
        n += __cajeta_pb_bytes(te + n, CAJ_PB_TE_NAME, (const uint8_t*) name, ln);
    }
    uint8_t pkt[700];
    int32_t p = __cajeta_pb_uint64(pkt, CAJ_PB_PKT_TIMESTAMP, ts);
    p += __cajeta_pb_uint64(pkt + p, CAJ_PB_PKT_SEQ_ID, seq_id);
    if (first_in_sequence)
        p += __cajeta_pb_uint64(pkt + p, CAJ_PB_PKT_SEQ_FLAGS, CAJ_PB_SEQ_FLAG_CLEARED);
    p += __cajeta_pb_bytes(pkt + p, CAJ_PB_PKT_TRACK_EVENT, te, n);
    return caj_pb_packet(out, pkt, p);
}
