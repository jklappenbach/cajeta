package dev.cajeta.idea.jsonl

import dev.cajeta.idea.debugger.Json

/**
 * CBOR (RFC 8949) for the viewer — the format chosen in spec 1.3.1a.
 *
 * Decoding accepts the whole format: every major type, definite and
 * indefinite lengths, tags, half/single/double floats, and CBOR Sequences
 * (RFC 8742), which are the binary shape of a JSONL stream — one row per item.
 *
 * Encoding emits only the DETERMINISTIC profile (§4.2): shortest-form integers
 * and lengths, definite lengths, map keys sorted by their encoded bytes. That
 * is what makes "decode then encode reproduces the file" a property of the
 * encoder rather than a hope (spec §4.1.4).
 *
 * The two are not symmetric, and that asymmetry is the point: a file using
 * anything outside the deterministic profile — an indefinite-length string, a
 * tag, a non-shortest integer, a byte string, a float we cannot reproduce
 * exactly — decodes fine for READING but reports `editable = false` with the
 * reason. Saving it would silently rewrite bytes the reader never asked to
 * change, and the spec's own alternative (§4.1.4) is a read-only file with the
 * reason shown.
 */
object CborCodec : BinaryJsonCodec {

    override val id: String = "cbor"
    override val extensions: Set<String> = setOf("cbor", "cborseq")

    /** RFC 8949 tag 55799 — the self-describe prefix, CBOR's magic number. */
    private val SELF_DESCRIBE = byteArrayOf(0xd9.toByte(), 0xd9.toByte(), 0xf7.toByte())

    /** Nesting bound. Deep nesting is how a hostile or truncated file turns a
     *  recursive decoder into a stack overflow; the limit is far above any
     *  real document. */
    private const val MAX_DEPTH = 512

    override fun detect(bytes: ByteArray, extension: String): Boolean {
        if (bytes.size >= 3 && bytes[0] == SELF_DESCRIBE[0] &&
            bytes[1] == SELF_DESCRIBE[1] && bytes[2] == SELF_DESCRIBE[2]) return true
        return extension in extensions
    }

    override fun decode(bytes: ByteArray): BinaryDecodeResult {
        val reader = Reader(bytes)
        val items = ArrayList<Json>()
        return try {
            // A sequence is simply items until the bytes run out (RFC 8742);
            // a single document is the one-item case, so one path serves both.
            while (!reader.atEnd()) items += reader.readValue(0)
            BinaryDecodeResult.Ok(items, reader.canonical, reader.reason)
        } catch (e: CborError) {
            BinaryDecodeResult.Err(e.offset, e.message ?: "malformed CBOR")
        }
    }

    override fun encode(value: Json): ByteArray {
        val out = ArrayList<Byte>(64)
        writeValue(out, value)
        return out.toByteArray()
    }

    /** Encode a whole sequence — the binary counterpart of writing JSONL. */
    fun encodeSequence(values: List<Json>): ByteArray {
        val out = ArrayList<Byte>(values.size * 32)
        for (v in values) writeValue(out, v)
        return out.toByteArray()
    }

    // --- decoding ---------------------------------------------------------

    private class CborError(val offset: Int, message: String) : Exception(message)

    private class Reader(private val b: ByteArray) {
        var pos = 0
        /** Still inside the deterministic profile? */
        var canonical = true
        var reason: String? = null

        fun atEnd(): Boolean = pos >= b.size

        private fun notCanonical(why: String) {
            if (canonical) { canonical = false; reason = why }
        }

        private fun need(n: Int) {
            // A length prefix is not a promise: a 4-byte header claiming two
            // gigabytes must fail here, at a cost of one comparison, rather
            // than in an allocator.
            if (n < 0 || pos + n > b.size)
                throw CborError(pos, "truncated: needs $n more byte(s), ${b.size - pos} left")
        }

        private fun u8(): Int { need(1); return b[pos++].toInt() and 0xff }

        private fun uint(n: Int): Long {
            need(n)
            var v = 0L
            repeat(n) { v = (v shl 8) or (b[pos++].toLong() and 0xff) }
            return v
        }

        /** The argument of an item: its length, or its value. -1 = indefinite. */
        private fun argument(info: Int, shortest: Long): Long = when {
            info < 24 -> info.toLong()
            info == 24 -> uint(1).also { if (it < 24) notCanonical("non-shortest integer encoding") }
            info == 25 -> uint(2).also { if (it < 256) notCanonical("non-shortest integer encoding") }
            info == 26 -> uint(4).also { if (it < 65536) notCanonical("non-shortest integer encoding") }
            info == 27 -> uint(8).also { if (it < 4294967296L) notCanonical("non-shortest integer encoding") }
            info == 31 -> -1L
            else -> throw CborError(pos - 1, "reserved additional information $info")
        }

        fun readValue(depth: Int): Json {
            if (depth > MAX_DEPTH) throw CborError(pos, "nesting deeper than $MAX_DEPTH")
            val start = pos
            val initial = u8()
            val major = (initial shr 5) and 0x07
            val info = initial and 0x1f
            return when (major) {
                0 -> {
                    val v = argument(info, 0)
                    exactInt(v, start)
                }
                1 -> {
                    val v = argument(info, 0)
                    // -1 - n, so the 64-bit negative range is fully usable.
                    if (v < 0) throw CborError(start, "indefinite length is not valid for an integer")
                    exactInt(-1L - v, start)
                }
                2 -> {
                    val bytes = readByteString(info, start, depth)
                    notCanonical("byte strings have no exact JSON form")
                    Json.Str(base64(bytes))
                }
                3 -> Json.Str(readTextString(info, start, depth))
                4 -> {
                    val n = argument(info, 0)
                    val arr = Json.Arr()
                    if (n < 0) {
                        notCanonical("indefinite-length array")
                        while (!breakHere()) arr.items += readValue(depth + 1)
                    } else {
                        boundedRepeat(n, start) { arr.items += readValue(depth + 1) }
                    }
                    arr
                }
                5 -> {
                    val n = argument(info, 0)
                    val obj = Json.Obj()
                    val keys = ArrayList<String>()
                    if (n < 0) {
                        notCanonical("indefinite-length map")
                        while (!breakHere()) readPair(obj, keys, depth)
                    } else {
                        boundedRepeat(n, start) { readPair(obj, keys, depth) }
                    }
                    if (keys != keys.sortedWith(::compareEncoded))
                        notCanonical("map keys are not in deterministic order")
                    obj
                }
                6 -> {
                    val tag = argument(info, 0)
                    if (tag < 0) throw CborError(start, "indefinite length is not valid for a tag")
                    // 55799 is the self-describe prefix and carries no data;
                    // every other tag is meaning we would drop on re-encode.
                    if (tag != 55799L) notCanonical("tag $tag cannot be re-encoded")
                    readValue(depth + 1)
                }
                else -> readSimpleOrFloat(info, start)
            }
        }

        private fun readPair(obj: Json.Obj, keys: ArrayList<String>, depth: Int) {
            val keyStart = pos
            val key = readValue(depth + 1)
            val name = when (key) {
                is Json.Str -> key.value
                // JSON objects are string-keyed; a non-string key is rendered
                // so it is at least visible, and locks the file read-only.
                else -> { notCanonical("map key that is not a text string"); render(key) }
            }
            if (obj.entries.containsKey(name)) notCanonical("duplicate map key '$name'")
            keys += name
            obj.entries[name] = readValue(depth + 1)
            if (keyStart == pos) throw CborError(keyStart, "empty map entry")
        }

        /** Repeat n times, but never trust n further than the bytes allow. */
        private inline fun boundedRepeat(n: Long, start: Int, body: () -> Unit) {
            if (n > b.size - pos) throw CborError(start, "declares $n item(s), only ${b.size - pos} byte(s) remain")
            var i = 0L
            while (i < n) { body(); i++ }
        }

        private fun breakHere(): Boolean {
            need(1)
            if ((b[pos].toInt() and 0xff) == 0xff) { pos++; return true }
            return false
        }

        private fun readTextString(info: Int, start: Int, depth: Int): String {
            val n = argument(info, 0)
            if (n < 0) {
                notCanonical("indefinite-length text string")
                val sb = StringBuilder()
                while (!breakHere()) {
                    val chunk = readValue(depth + 1)
                    sb.append((chunk as? Json.Str)?.value ?: throw CborError(pos, "bad text chunk"))
                }
                return sb.toString()
            }
            if (n > b.size - pos) throw CborError(start, "text of $n byte(s), only ${b.size - pos} remain")
            val s = String(b, pos, n.toInt(), Charsets.UTF_8)
            pos += n.toInt()
            return s
        }

        private fun readByteString(info: Int, start: Int, depth: Int): ByteArray {
            val n = argument(info, 0)
            if (n < 0) {
                val acc = ArrayList<Byte>()
                while (!breakHere()) {
                    val chunkStart = pos
                    val chunk = readValue(depth + 1)
                    if (chunk !is Json.Str) throw CborError(chunkStart, "bad byte chunk")
                    acc.addAll(unBase64(chunk.value).toList())
                }
                return acc.toByteArray()
            }
            if (n > b.size - pos) throw CborError(start, "bytes of $n, only ${b.size - pos} remain")
            val out = b.copyOfRange(pos, pos + n.toInt())
            pos += n.toInt()
            return out
        }

        private fun readSimpleOrFloat(info: Int, start: Int): Json = when (info) {
            20 -> Json.Bool(false)
            21 -> Json.Bool(true)
            22 -> Json.Null
            23 -> { notCanonical("undefined has no JSON form"); Json.Null }
            25 -> { notCanonical("half-precision float"); Json.Num(half(uint(2).toInt()), false) }
            26 -> { notCanonical("single-precision float"); Json.Num(
                        java.lang.Float.intBitsToFloat(uint(4).toInt()).toDouble(), false) }
            27 -> Json.Num(java.lang.Double.longBitsToDouble(uint(8)), false)
            31 -> throw CborError(start, "unexpected break")
            else -> { notCanonical("simple value $info"); Json.Num(argument(info, 0).toDouble(), true) }
        }

        /** Integers beyond 2^53 cannot survive the Double-backed value model,
         *  so the file stays readable but not editable rather than quietly
         *  rounding somebody's identifier. */
        private fun exactInt(v: Long, start: Int): Json {
            if (v > 9007199254740992L || v < -9007199254740992L)
                notCanonical("integer $v exceeds exact double precision")
            return Json.Num(v.toDouble(), true)
        }
    }

    // --- encoding (deterministic profile only) -----------------------------

    private fun writeValue(out: ArrayList<Byte>, v: Json) {
        when (v) {
            is Json.Null -> out.add(0xf6.toByte())
            is Json.Bool -> out.add(if (v.value) 0xf5.toByte() else 0xf4.toByte())
            is Json.Num ->
                if (v.isInt) writeInt(out, v.value.toLong())
                else { out.add(0xfb.toByte()); writeBits(out, java.lang.Double.doubleToRawLongBits(v.value), 8) }
            is Json.Str -> {
                val utf8 = v.value.toByteArray(Charsets.UTF_8)
                writeHead(out, 3, utf8.size.toLong())
                for (byte in utf8) out.add(byte)
            }
            is Json.Arr -> {
                writeHead(out, 4, v.items.size.toLong())
                for (item in v.items) writeValue(out, item)
            }
            is Json.Obj -> {
                writeHead(out, 5, v.entries.size.toLong())
                // Deterministic encoding orders keys by their ENCODED bytes,
                // which is length-then-lexicographic for text keys, not the
                // insertion order the tree happens to hold.
                for (k in v.entries.keys.sortedWith(::compareEncoded)) {
                    writeValue(out, Json.Str(k))
                    writeValue(out, v.entries[k]!!)
                }
            }
        }
    }

    private fun writeInt(out: ArrayList<Byte>, value: Long) {
        if (value >= 0) writeHead(out, 0, value) else writeHead(out, 1, -1L - value)
    }

    private fun writeHead(out: ArrayList<Byte>, major: Int, arg: Long) {
        val m = major shl 5
        when {
            arg < 24 -> out.add((m or arg.toInt()).toByte())
            arg < 256 -> { out.add((m or 24).toByte()); writeBits(out, arg, 1) }
            arg < 65536 -> { out.add((m or 25).toByte()); writeBits(out, arg, 2) }
            arg < 4294967296L -> { out.add((m or 26).toByte()); writeBits(out, arg, 4) }
            else -> { out.add((m or 27).toByte()); writeBits(out, arg, 8) }
        }
    }

    private fun writeBits(out: ArrayList<Byte>, value: Long, bytes: Int) {
        for (i in bytes - 1 downTo 0) out.add(((value shr (i * 8)) and 0xff).toByte())
    }

    // --- shared helpers ----------------------------------------------------

    /** RFC 8949 §4.2.1 key order: shorter encoding first, then bytewise. */
    private fun compareEncoded(a: String, b: String): Int {
        val ab = a.toByteArray(Charsets.UTF_8)
        val bb = b.toByteArray(Charsets.UTF_8)
        if (ab.size != bb.size) return ab.size - bb.size
        for (i in ab.indices) {
            val d = (ab[i].toInt() and 0xff) - (bb[i].toInt() and 0xff)
            if (d != 0) return d
        }
        return 0
    }

    private fun half(bits: Int): Double {
        val sign = if (bits and 0x8000 != 0) -1.0 else 1.0
        val exp = (bits shr 10) and 0x1f
        val frac = bits and 0x03ff
        return when (exp) {
            0 -> sign * Math.pow(2.0, -14.0) * (frac / 1024.0)
            31 -> if (frac == 0) sign * Double.POSITIVE_INFINITY else Double.NaN
            else -> sign * Math.pow(2.0, (exp - 15).toDouble()) * (1 + frac / 1024.0)
        }
    }

    private fun render(v: Json): String = when (v) {
        is Json.Str -> v.value
        else -> v.toCompactString()
    }

    private val B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

    private fun base64(data: ByteArray): String {
        val sb = StringBuilder((data.size + 2) / 3 * 4)
        var i = 0
        while (i + 2 < data.size) {
            val n = ((data[i].toInt() and 0xff) shl 16) or
                    ((data[i + 1].toInt() and 0xff) shl 8) or (data[i + 2].toInt() and 0xff)
            sb.append(B64[n shr 18 and 63]).append(B64[n shr 12 and 63])
              .append(B64[n shr 6 and 63]).append(B64[n and 63])
            i += 3
        }
        when (data.size - i) {
            1 -> {
                val n = (data[i].toInt() and 0xff) shl 16
                sb.append(B64[n shr 18 and 63]).append(B64[n shr 12 and 63]).append("==")
            }
            2 -> {
                val n = ((data[i].toInt() and 0xff) shl 16) or ((data[i + 1].toInt() and 0xff) shl 8)
                sb.append(B64[n shr 18 and 63]).append(B64[n shr 12 and 63])
                  .append(B64[n shr 6 and 63]).append('=')
            }
        }
        return sb.toString()
    }

    private fun unBase64(s: String): ByteArray {
        val clean = s.filter { it != '=' }
        val out = ArrayList<Byte>(clean.length * 3 / 4)
        var buffer = 0
        var bits = 0
        for (c in clean) {
            val v = B64.indexOf(c)
            if (v < 0) continue
            buffer = (buffer shl 6) or v
            bits += 6
            if (bits >= 8) { bits -= 8; out.add(((buffer shr bits) and 0xff).toByte()) }
        }
        return out.toByteArray()
    }
}
