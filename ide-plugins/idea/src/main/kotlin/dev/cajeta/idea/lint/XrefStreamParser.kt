package dev.cajeta.idea.lint

import dev.cajeta.idea.debugger.Json

/**
 * One xref record from the lint stream (ide-symbol-index §2.0.2):
 * `{"kind":"xref","rel":"<relation>","record":{...}}`. `rel` names the relation
 * (`declarations`, `inheritance`, `references`, `overrides`, `calls` — or one
 * this plugin does not know yet; a minor schema bump may add relations, and an
 * unknown `rel` is data to ignore at USE time, not a parse failure). `record` is
 * the schema record verbatim — the same shape the whole-root document carries,
 * so ingest code reads both feeds with one reader.
 */
data class XrefRecord(val rel: String, val record: Json.Obj)

/**
 * The xref side of a demultiplexed lint stream.
 *
 * [supported] is the version handshake's verdict (spec §2.0.6): when the stream
 * declares a major this plugin does not speak — or declares none at all while
 * carrying records — the records are REFUSED wholesale, never partially read.
 * A half-understood index is the wrong-answer machine this design exists to
 * prevent; diagnostics are unaffected either way.
 */
data class XrefStream(
    val records: List<XrefRecord>,
    val versionMajor: Int?,
    val supported: Boolean,
) {
    companion object {
        val EMPTY = XrefStream(emptyList(), null, supported = true)
    }
}

/**
 * Demultiplexes the `--emit-xref` lint stream out of the NDJSON stderr the
 * compiler already produces (json-diagnostics spec §3). Tolerant by contract,
 * like [dev.cajeta.idea.buildtool.JsonDiagnosticParser]: a line that is not a
 * `kind:"xref"` object — a diagnostic, a progress record, a future record kind,
 * console noise — is simply not ours, never an error. Pure / off-platform.
 */
object XrefStreamParser {

    /** The schema major this plugin can read (cajeta-xref-v1.schema.json). */
    const val SUPPORTED_MAJOR = 1

    /** Parse one NDJSON line; null unless it is a well-formed xref record. */
    fun parseLine(line: String): XrefRecord? {
        val trimmed = line.trim()
        if (trimmed.isEmpty() || trimmed[0] != '{') return null
        val root = try {
            Json.parse(trimmed)
        } catch (_: Exception) {
            return null
        }
        if (root !is Json.Obj) return null
        if ((root.opt("kind") as? Json.Str)?.value != "xref") return null
        val rel = (root.opt("rel") as? Json.Str)?.value ?: return null
        val record = root.opt("record") as? Json.Obj ?: return null
        return XrefRecord(rel, record)
    }

    /**
     * Demux a whole lint stderr: every xref record, gated by the version
     * handshake. The compiler emits the version record before any other (the
     * stream writer's contract), but this reader does not depend on the order —
     * it collects, then judges.
     */
    fun demux(stderr: String): XrefStream {
        var major: Int? = null
        val records = mutableListOf<XrefRecord>()
        for (line in stderr.lineSequence()) {
            val r = parseLine(line) ?: continue
            if (r.rel == "version") {
                major = (r.record.opt("major") as? Json.Num)?.value?.toInt()
            } else {
                records += r
            }
        }
        if (records.isEmpty()) return XrefStream(emptyList(), major, supported = true)

        // Records with no version line at all are as unreadable as a wrong
        // major: nothing vouches for their shape (§2.0.6 — refuse, don't guess).
        val ok = major == SUPPORTED_MAJOR
        return XrefStream(if (ok) records else emptyList(), major, ok)
    }
}
