package dev.cajeta.idea.jsonl

import dev.cajeta.idea.debugger.Json

/**
 * The binary-JSON decoder seam (json-viewer spec §2.1.5). A codec turns bytes
 * into the shared value tree and back; registration is data-driven, so a
 * second format plugs in without the viewer changing. CBOR is the first codec
 * behind it (spec 1.3.1a), not a replacement for it.
 */
interface BinaryJsonCodec {
    /** Stable id, used in messages and in the registry. */
    val id: String

    /** File extensions this codec claims, lower-case, without the dot. */
    val extensions: Set<String>

    /** True when these bytes look like this format. Content beats extension:
     *  a codec with a magic number should say so from the bytes alone. */
    fun detect(bytes: ByteArray, extension: String): Boolean

    fun decode(bytes: ByteArray): BinaryDecodeResult

    /** Encode one value. Deterministic where the format defines determinism. */
    fun encode(value: Json): ByteArray
}

/**
 * What a decode produced. [items] holds one entry per top-level value — a
 * lone document is one item, a SEQUENCE (RFC 8742 for CBOR) is many, which is
 * the binary analogue of JSONL and renders as one row each.
 *
 * [editable] is the honest answer to "can this file be saved back": true only
 * when re-encoding the decoded tree reproduces the input byte for byte. The
 * spec allows either byte-stability or a read-only file with the reason shown
 * (§4.1.4), and the second is what constructs outside the deterministic
 * profile get — see [reason].
 */
sealed class BinaryDecodeResult {
    data class Ok(
        val items: List<Json>,
        val editable: Boolean,
        /** Why editing is off, when it is. Shown to the reader verbatim. */
        val reason: String? = null,
    ) : BinaryDecodeResult()

    /** A located failure. [offset] is the byte the decoder choked on, so the
     *  reader is pointed at the damage rather than told "corrupt file". */
    data class Err(val offset: Int, val message: String) : BinaryDecodeResult()
}

/** The registered codecs (§5.1.5). Extension-first, then content sniffing. */
object BinaryJsonCodecs {
    private val codecs: List<BinaryJsonCodec> = listOf(CborCodec)

    fun byId(id: String): BinaryJsonCodec? = codecs.firstOrNull { it.id == id }

    /** The codec for a file, or null when nothing claims it — in which case
     *  the viewer leaves the file alone rather than guessing. */
    fun forFile(fileName: String, bytes: ByteArray): BinaryJsonCodec? {
        val ext = fileName.substringAfterLast('.', "").lowercase()
        return codecs.firstOrNull { it.detect(bytes, ext) }
    }

    fun claimsExtension(extension: String): Boolean =
        codecs.any { extension.lowercase() in it.extensions }
}
