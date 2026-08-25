package dev.cajeta.idea.profiler

/**
 * cajeta-profiler Unit 11.2.a — a protobuf wire reader, by hand.
 *
 * The viewer decodes Perfetto traces without generated classes and without a
 * protobuf dependency. The reason is version coupling, not size: the IntelliJ
 * platform ships `protobuf-java 3.24.4-jb.2` — a JetBrains fork — on the
 * platform classpath, and this plugin declares 242 -> 299.* on purpose so
 * routine IDE updates do not disable it. Generated protobuf code is coupled to
 * its runtime (4.x renamed `GeneratedMessageV3` to `GeneratedMessage`), so
 * generated classes would pin the viewer to one line of a fork nobody here
 * controls, across that whole range. Bundling our own copy is what
 * `build.gradle.kts` already refuses to do for markdown.
 *
 * The trace writer hand-encodes for the same reason (profiler spec §7, plan
 * 5.3.b: no SDK, no generated code, no build dependency), so reader and writer
 * are symmetric.
 *
 * Only three wire types appear in these traces — varint, length-delimited, and
 * fixed64 for the two flow fields — but [skip] handles all five, because a
 * trace from a newer writer will carry fields this build has never heard of and
 * the viewer has to keep going rather than fail on a valid file.
 *
 * Field numbers are pinned in `third_party/perfetto/PROVENANCE.md`, verified
 * against the vendored schema rather than recalled.
 */
class ProtoWire(
    private val buf: ByteArray,
    private var pos: Int = 0,
    private val end: Int = buf.size,
) {

    /** A field claimed more bytes than the buffer holds. */
    class Truncated(message: String) : RuntimeException(message)

    companion object {
        const val VARINT = 0
        const val FIXED64 = 1
        const val LEN = 2
        const val START_GROUP = 3
        const val END_GROUP = 4
        const val FIXED32 = 5

        fun fieldOf(tag: Int): Int = tag ushr 3
        fun wireOf(tag: Int): Int = tag and 7
    }

    fun hasMore(): Boolean = pos < end

    /** Field number and wire type, packed as protobuf packs them. */
    fun readTag(): Int = readVarint().toInt()

    /**
     * Base-128, seven bits per byte, least significant group first.
     *
     * Ten bytes are read rather than nine: 2^64-1 needs all ten, and the
     * profiler generates exactly those values — track uuids are built by
     * shifting a marker into the top bits, so the high bit is set on every one
     * of them. A reader that stopped at nine would lose precisely the ids it
     * most needs to get right.
     */
    fun readVarint(): Long {
        var result = 0L
        var shift = 0
        while (shift < 64) {
            if (pos >= end) throw Truncated("varint runs past the end of the buffer")
            val b = buf[pos++].toInt()
            result = result or ((b.toLong() and 0x7F) shl shift)
            if (b and 0x80 == 0) return result
            shift += 7
        }
        throw Truncated("varint longer than ten bytes")
    }

    /**
     * Eight little-endian bytes. `TrackEvent.flow_ids` (47) and
     * `terminating_flow_ids` (48) are the only fixed64 fields the writer emits,
     * and their deprecated varint twins (36, 42) still exist in the schema — so
     * reading one as a varint produces a packet that parses cleanly and a flow
     * that never appears.
     */
    fun readFixed64(): Long {
        if (pos + 8 > end) throw Truncated("fixed64 runs past the end of the buffer")
        var v = 0L
        for (i in 0 until 8) v = v or ((buf[pos + i].toLong() and 0xFF) shl (8 * i))
        pos += 8
        return v
    }

    fun readFixed32(): Int {
        if (pos + 4 > end) throw Truncated("fixed32 runs past the end of the buffer")
        var v = 0
        for (i in 0 until 4) v = v or ((buf[pos + i].toInt() and 0xFF) shl (8 * i))
        pos += 4
        return v
    }

    fun readBytes(): ByteArray {
        val len = readVarint().toInt()
        if (len < 0 || pos + len > end) throw Truncated("length-delimited field claims $len bytes")
        val out = buf.copyOfRange(pos, pos + len)
        pos += len
        return out
    }

    fun readString(): String = String(readBytes(), Charsets.UTF_8)

    /**
     * A reader over the next length-delimited field, bounded by that field.
     * Bounding matters: a sub-reader that ran past its own end would read its
     * parent's next field as one of its own, and every packet in a trace is a
     * nest of these.
     */
    fun readSub(): ProtoWire {
        val len = readVarint().toInt()
        if (len < 0 || pos + len > end) throw Truncated("sub-message claims $len bytes")
        val sub = ProtoWire(buf, pos, pos + len)
        pos += len
        return sub
    }

    /** Step over a field of the given wire type without interpreting it. */
    fun skip(wireType: Int) {
        when (wireType) {
            VARINT -> readVarint()
            FIXED64 -> readFixed64()
            LEN -> readBytes()
            FIXED32 -> readFixed32()
            START_GROUP -> {
                // Groups are deprecated and Perfetto emits none, but skipping
                // one correctly costs four lines and failing on one costs the
                // whole file.
                while (hasMore()) {
                    val tag = readTag()
                    if (wireOf(tag) == END_GROUP) return
                    skip(wireOf(tag))
                }
                throw Truncated("group is not terminated")
            }
            END_GROUP -> Unit
            else -> throw Truncated("unknown wire type $wireType")
        }
    }
}
