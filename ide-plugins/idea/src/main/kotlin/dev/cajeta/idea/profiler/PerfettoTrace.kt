package dev.cajeta.idea.profiler

/**
 * cajeta-profiler Unit 11 — the trace, as the viewer needs it.
 *
 * A narrow view of Perfetto's schema: the messages `cajeta-profiler` actually
 * writes, and nothing else. Field numbers come from
 * `third_party/perfetto/PROVENANCE.md`, which pins them against the vendored
 * schema.
 */

/** A timeline lane: a thread, a fiber, or a device queue (spec §8.3). */
data class ProfileTrack(
    val uuid: Long,
    val name: String,
    val parentUuid: Long = 0,
)

/** Where a frame came from, so selecting it can navigate there (spec §8.2). */
data class ProfileSourceLocation(
    val iid: Long,
    val fileName: String,
    val functionName: String,
    val line: Int,
)

data class ProfileEvent(
    val trackUuid: Long,
    val type: Int,
    val timestampNs: Long,
    val name: String?,
    val sourceLocation: ProfileSourceLocation?,
    val annotations: Map<String, String> = emptyMap(),
    /**
     * Interning ids, kept on the event until the second pass resolves them.
     * They stay visible rather than being hidden in a side table because an
     * unresolved id is diagnostic: a name that never resolved means the
     * interned entry never arrived, and that is worth being able to see.
     */
    val nameIid: Long = 0,
    val sourceLocationIid: Long = 0,
) {
    val isBegin: Boolean get() = type == PerfettoTraceReader.TYPE_SLICE_BEGIN
    val isEnd: Boolean get() = type == PerfettoTraceReader.TYPE_SLICE_END
    val isInstant: Boolean get() = type == PerfettoTraceReader.TYPE_INSTANT
}

data class ProfileTrace(
    val tracks: List<ProfileTrack>,
    val events: List<ProfileEvent>,
    val packetCount: Int,
    /** Packets that could not be decoded — a truncated tail, usually. */
    val truncated: Boolean = false,
)

/**
 * Reads a `.pftrace` into [ProfileTrace].
 *
 * Two behaviours are deliberate and both are about opening files that are not
 * pristine:
 *
 * A **truncated** trace yields the packets it did contain. A profiled process
 * can be killed mid-write, and the packets already on disk are still a valid
 * measurement; discarding all of them because the last one is half-written
 * would be the worst available response.
 *
 * **Unknown fields are skipped.** A trace from a newer writer carries fields
 * this build has never heard of, and it is still a trace.
 */
object PerfettoTraceReader {

    const val TYPE_SLICE_BEGIN = 1
    const val TYPE_SLICE_END = 2
    const val TYPE_INSTANT = 3

    // Trace
    private const val TRACE_PACKET = 1

    // TracePacket
    private const val PKT_TIMESTAMP = 8
    private const val PKT_TRACK_EVENT = 11
    private const val PKT_INTERNED_DATA = 12
    private const val PKT_TRACK_DESCRIPTOR = 60

    // TrackDescriptor
    private const val TD_UUID = 1
    private const val TD_NAME = 2
    private const val TD_PARENT_UUID = 5

    // TrackEvent
    private const val TE_DEBUG_ANNOTATIONS = 4
    private const val TE_TYPE = 9
    private const val TE_NAME_IID = 10
    private const val TE_TRACK_UUID = 11
    private const val TE_NAME = 23
    private const val TE_SOURCE_LOCATION_IID = 34

    // InternedData
    private const val ID_EVENT_NAMES = 2
    private const val ID_SOURCE_LOCATIONS = 4

    // EventName / SourceLocation
    private const val EN_IID = 1
    private const val EN_NAME = 2
    private const val SL_IID = 1
    private const val SL_FILE_NAME = 2
    private const val SL_FUNCTION_NAME = 3
    private const val SL_LINE_NUMBER = 4

    // DebugAnnotation
    private const val DA_INT_VALUE = 4
    private const val DA_STRING_VALUE = 6
    private const val DA_NAME = 10

    fun read(bytes: ByteArray): ProfileTrace {
        val tracks = LinkedHashMap<Long, ProfileTrack>()
        val events = ArrayList<ProfileEvent>()
        // Interned tables accumulate across the file. They are scoped to a
        // packet sequence, and the writer keeps ONE sequence for the whole
        // trace (PROVENANCE.md: splitting it would unbind every device clock
        // from its snapshot), so one table per file is correct here and would
        // not be for a trace from a multi-sequence producer.
        val eventNames = HashMap<Long, String>()
        val sourceLocations = HashMap<Long, ProfileSourceLocation>()

        var packets = 0
        var truncated = false
        val r = ProtoWire(bytes)
        try {
            while (r.hasMore()) {
                val tag = r.readTag()
                if (ProtoWire.fieldOf(tag) != TRACE_PACKET || ProtoWire.wireOf(tag) != ProtoWire.LEN) {
                    r.skip(ProtoWire.wireOf(tag))
                    continue
                }
                val pkt = r.readSub()
                packets++
                readPacket(pkt, tracks, events, eventNames, sourceLocations)
            }
        } catch (e: ProtoWire.Truncated) {
            truncated = true
        }

        // Names and locations are resolved in a second pass. An event may be
        // written before the packet that interns its name — the writer emits
        // interned data alongside first use, but nothing in the format promises
        // that order, and resolving inline would drop every name that arrived
        // late while still parsing cleanly.
        val resolved = events.map { e ->
            if ((e.name != null || e.nameIid == 0L) && e.sourceLocationIid == 0L) e
            else e.copy(
                name = e.name ?: eventNames[e.nameIid],
                sourceLocation = sourceLocations[e.sourceLocationIid],
            )
        }
        return ProfileTrace(tracks.values.toList(), resolved, packets, truncated)
    }

    private fun readPacket(
        pkt: ProtoWire,
        tracks: MutableMap<Long, ProfileTrack>,
        events: MutableList<ProfileEvent>,
        eventNames: MutableMap<Long, String>,
        sourceLocations: MutableMap<Long, ProfileSourceLocation>,
    ) {
        var timestamp = 0L
        var trackEvent: ProtoWire? = null
        while (pkt.hasMore()) {
            val tag = pkt.readTag()
            when (ProtoWire.fieldOf(tag)) {
                PKT_TIMESTAMP -> timestamp = pkt.readVarint()
                PKT_TRACK_DESCRIPTOR -> readTrackDescriptor(pkt.readSub(), tracks)
                PKT_INTERNED_DATA -> readInterned(pkt.readSub(), eventNames, sourceLocations)
                // Held rather than parsed in place: the packet's timestamp may
                // arrive after the event, and the event needs it.
                PKT_TRACK_EVENT -> trackEvent = pkt.readSub()
                else -> pkt.skip(ProtoWire.wireOf(tag))
            }
        }
        trackEvent?.let { events.add(readTrackEvent(it, timestamp)) }
    }

    private fun readTrackDescriptor(td: ProtoWire, tracks: MutableMap<Long, ProfileTrack>) {
        var uuid = 0L
        var name = ""
        var parent = 0L
        while (td.hasMore()) {
            val tag = td.readTag()
            when (ProtoWire.fieldOf(tag)) {
                TD_UUID -> uuid = td.readVarint()
                TD_NAME -> name = td.readString()
                TD_PARENT_UUID -> parent = td.readVarint()
                else -> td.skip(ProtoWire.wireOf(tag))
            }
        }
        if (uuid != 0L) tracks[uuid] = ProfileTrack(uuid, name, parent)
    }

    private fun readInterned(
        interned: ProtoWire,
        eventNames: MutableMap<Long, String>,
        sourceLocations: MutableMap<Long, ProfileSourceLocation>,
    ) {
        while (interned.hasMore()) {
            val tag = interned.readTag()
            when (ProtoWire.fieldOf(tag)) {
                ID_EVENT_NAMES -> {
                    val en = interned.readSub()
                    var iid = 0L
                    var name = ""
                    while (en.hasMore()) {
                        val t = en.readTag()
                        when (ProtoWire.fieldOf(t)) {
                            EN_IID -> iid = en.readVarint()
                            EN_NAME -> name = en.readString()
                            else -> en.skip(ProtoWire.wireOf(t))
                        }
                    }
                    if (iid != 0L) eventNames[iid] = name
                }
                ID_SOURCE_LOCATIONS -> {
                    val sl = interned.readSub()
                    var iid = 0L
                    var file = ""
                    var fn = ""
                    var line = 0
                    while (sl.hasMore()) {
                        val t = sl.readTag()
                        when (ProtoWire.fieldOf(t)) {
                            SL_IID -> iid = sl.readVarint()
                            SL_FILE_NAME -> file = sl.readString()
                            SL_FUNCTION_NAME -> fn = sl.readString()
                            SL_LINE_NUMBER -> line = sl.readVarint().toInt()
                            else -> sl.skip(ProtoWire.wireOf(t))
                        }
                    }
                    if (iid != 0L) sourceLocations[iid] = ProfileSourceLocation(iid, file, fn, line)
                }
                else -> interned.skip(ProtoWire.wireOf(tag))
            }
        }
    }

    private fun readTrackEvent(te: ProtoWire, timestamp: Long): ProfileEvent {
        var type = 0
        var trackUuid = 0L
        var nameIid = 0L
        var name: String? = null
        var locIid = 0L
        val annotations = LinkedHashMap<String, String>()
        while (te.hasMore()) {
            val tag = te.readTag()
            when (ProtoWire.fieldOf(tag)) {
                TE_TYPE -> type = te.readVarint().toInt()
                TE_TRACK_UUID -> trackUuid = te.readVarint()
                TE_NAME_IID -> nameIid = te.readVarint()
                TE_NAME -> name = te.readString()
                TE_SOURCE_LOCATION_IID -> locIid = te.readVarint()
                TE_DEBUG_ANNOTATIONS -> readAnnotation(te.readSub(), annotations)
                else -> te.skip(ProtoWire.wireOf(tag))
            }
        }
        return ProfileEvent(
            trackUuid = trackUuid,
            type = type,
            timestampNs = timestamp,
            name = name,
            sourceLocation = null,
            annotations = annotations,
            nameIid = nameIid,
            sourceLocationIid = locIid,
        )
    }

    private fun readAnnotation(da: ProtoWire, into: MutableMap<String, String>) {
        var name = ""
        var value: String? = null
        while (da.hasMore()) {
            val tag = da.readTag()
            when (ProtoWire.fieldOf(tag)) {
                DA_NAME -> name = da.readString()
                DA_INT_VALUE -> value = da.readVarint().toString()
                DA_STRING_VALUE -> value = da.readString()
                else -> da.skip(ProtoWire.wireOf(tag))
            }
        }
        if (name.isNotEmpty() && value != null) into[name] = value
    }
}
