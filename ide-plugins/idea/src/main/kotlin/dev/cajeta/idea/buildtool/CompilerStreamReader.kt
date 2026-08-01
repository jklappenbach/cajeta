package dev.cajeta.idea.buildtool

import dev.cajeta.idea.debugger.Json

/**
 * One record off the compiler's NDJSON stream (compiler-jsonl §2/§3), kept as
 * its `kind` plus the parsed object.
 *
 * Deliberately NOT a sealed hierarchy of per-kind payload types. The reader's
 * job is negotiation — version, dispatch, tolerance — and modelling payloads
 * here would mean inventing types for record kinds that do not exist yet
 * (`log` and `result` arrive in Unit 3). Consumers read the fields they need.
 */
data class CompilerRecord(val kind: String, val obj: Json.Obj) {
    /** String field, or null when absent / JSON null / another type. */
    fun str(key: String): String? = (obj.opt(key) as? Json.Str)?.value

    /** Int field, or null when absent / JSON null / another type. */
    fun int(key: String): Int? = (obj.opt(key) as? Json.Num)?.value?.toInt()
}

/**
 * A whole compiler stream, read and judged.
 *
 * [supported] is the version verdict and it is ONE property of the stream, not
 * a per-record flag — a consumer therefore cannot report a refusal once per
 * line even by accident (spec 2.3.1). When it is false, [records] is empty:
 * refusal is wholesale, because the point of a major bump is precisely that
 * recognising individual records is no longer reliable (2.1.4).
 *
 * [legacy] marks a stream from a compiler older than the envelope — no
 * `stream` record, and diagnostics with no `kind`. Those are still parsed
 * (spec 6.2.2): the plugin and the compiler ship separately, so a new plugin
 * meets old compilers routinely.
 */
data class CompilerStream(
    val records: List<CompilerRecord>,
    val versionMajor: Int?,
    val versionMinor: Int?,
    val producer: String?,
    val supported: Boolean,
    val legacy: Boolean,
) {
    /** Records whose kind this plugin understands. */
    val known: List<CompilerRecord>
        get() = records.filter { it.kind in CompilerStreamReader.KNOWN_KINDS }

    /** Records carrying a kind added after this plugin was built (2.1.5). */
    val unknown: List<CompilerRecord>
        get() = records.filterNot { it.kind in CompilerStreamReader.KNOWN_KINDS }

    companion object {
        val EMPTY = CompilerStream(
            emptyList(), null, null, null, supported = true, legacy = true)
    }
}

/**
 * Reads the compiler's `--diag-format=json` stream and applies the version
 * handshake, modelled on [dev.cajeta.idea.lint.XrefStreamParser] — the
 * strongest of the conventions this envelope consolidates.
 *
 * Tolerant everywhere except the major: non-JSON console noise is skipped (the
 * compiler's stderr is a shared channel and always has been), an unknown kind
 * is carried but not claimed as known, and unknown fields are simply never
 * read. Pure / off-platform.
 */
object CompilerStreamReader {

    /** The schema major this plugin can read (compiler-jsonl 2.1.4). */
    const val SUPPORTED_MAJOR = 1

    /** Record kinds this plugin understands today. Anything else is [unknown]
     *  — skipped by consumers, never a parse failure (2.1.5). */
    val KNOWN_KINDS = setOf(
        "diagnostic", "progress", "cache", "xref", "log", "result")

    fun read(stderr: String): CompilerStream {
        var major: Int? = null
        var minor: Int? = null
        var producer: String? = null
        var sawStreamRecord = false
        val records = mutableListOf<CompilerRecord>()

        for (line in stderr.lineSequence()) {
            val trimmed = line.trim()
            if (trimmed.isEmpty() || trimmed[0] != '{') continue
            val root = try {
                Json.parse(trimmed)
            } catch (_: Exception) {
                continue
            }
            if (root !is Json.Obj) continue

            val kind = (root.opt("kind") as? Json.Str)?.value
                // Pre-envelope compilers emitted diagnostics with no `kind`,
                // recognised by their having a `severity` (spec 6.2.2). This
                // is the ONE inference the reader makes, and only for the
                // legacy shape.
                ?: if (root.opt("severity") is Json.Str) "diagnostic" else continue

            if (kind == "stream") {
                sawStreamRecord = true
                major = (root.opt("major") as? Json.Num)?.value?.toInt()
                minor = (root.opt("minor") as? Json.Num)?.value?.toInt()
                producer = (root.opt("producer") as? Json.Str)?.value
                continue    // the announcement is not itself a payload record
            }
            records += CompilerRecord(kind, root)
        }

        // A stream that never announced itself predates the envelope. Refusing
        // it would break every user whose plugin outran their compiler.
        if (!sawStreamRecord) {
            return CompilerStream(records, null, null, null,
                                  supported = true, legacy = true)
        }
        // Announced, but in a dialect we do not speak: refuse the whole thing.
        val supported = major == SUPPORTED_MAJOR
        return CompilerStream(
            records = if (supported) records else emptyList(),
            versionMajor = major,
            versionMinor = minor,
            producer = producer,
            supported = supported,
            legacy = false,
        )
    }
}
