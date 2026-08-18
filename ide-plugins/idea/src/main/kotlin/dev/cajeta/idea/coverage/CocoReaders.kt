package dev.cajeta.idea.coverage

/**
 * Readers for cajeta-coco's published artifacts, written against
 * `docs/formats.md` and the conformance fixture in that repo.
 *
 * ## Refuse, do not cope
 *
 * An unrecognised version is refused, never parsed on a guess. Probe ids are
 * positional against the site table, so a profile read under the wrong
 * assumptions attributes hits to the *wrong sites* — the IDE then paints lines
 * green that never ran, and nothing downstream can tell. A refusal is
 * recoverable; a plausible wrong number is not. coco enforces the same rule on
 * its side.
 *
 * ## Truncated is not malformed
 *
 * These two are deliberately treated differently:
 *
 *  - a **malformed** record — a complete line with the wrong shape — is a
 *    corrupt artifact, and is refused;
 *  - a **truncated** trailing line — no terminating newline, because the writer
 *    is still going — is not an error. Parsing stops at the last complete
 *    record, which is what lets a live view read a file mid-write
 *    (plan 2.1.c, and the prerequisite for the live view in spec §9.2).
 */

class CocoFormatException(message: String) : Exception(message)

private const val SITES_HEADER = "coco-sites v1"
private const val PROFILE_HEADER = "coco-profile v1"
private const val ATTRIBUTION_HEADER = "coco-attribution v1"

/**
 * Split into records, dropping an unterminated trailing line.
 *
 * A file that does not end in a newline has a final record still being written;
 * it is excluded rather than parsed half-formed.
 */
private fun completeLines(text: String): List<String> {
    if (text.isEmpty()) return emptyList()
    val lines = text.split('\n')
    // split() yields a trailing "" when the text ends in '\n'; anything else in
    // that slot is a partial record.
    return lines.dropLast(1)
}

private fun requireHeader(lines: List<String>, expected: String, what: String) {
    val header = lines.firstOrNull()
        ?: throw CocoFormatException("$what: empty document; expected header \"$expected\"")
    if (header != expected) {
        throw CocoFormatException(
            "$what: unsupported header \"$header\"; this build reads only \"$expected\""
        )
    }
}

/** Parse a `coco-sites v1` document. */
fun parseCocoSites(text: String): List<CocoSite> {
    val lines = completeLines(text)
    requireHeader(lines, SITES_HEADER, "coco-sites")

    val out = ArrayList<CocoSite>(lines.size)
    for (i in 1 until lines.size) {
        val line = lines[i]
        if (line.isEmpty()) continue
        val lineNo = i + 1 // lines[0] is line 1
        val f = line.split('\t')
        if (f.size < 9) {
            throw CocoFormatException(
                "coco-sites: malformed row at line $lineNo; " +
                    "expected 9 tab-separated fields, found ${f.size}"
            )
        }
        val kind = CocoSiteKind.of(f[1])
            ?: throw CocoFormatException(
                "coco-sites: unknown site kind \"${f[1]}\" at line $lineNo"
            )
        out.add(
            CocoSite(
                id = f[0].toLongOrNull()
                    ?: throw CocoFormatException("coco-sites: non-numeric id at line $lineNo"),
                kind = kind,
                line = f[2].toIntOrNull()
                    ?: throw CocoFormatException("coco-sites: non-numeric line at line $lineNo"),
                decision = f[3].toLongOrNull() ?: -1L,
                file = f[4],
                owner = f[5],
                method = f[6],
                block = f[7],
                target = f[8],
            )
        )
    }
    return out
}

/**
 * Parse a `coco-profile v1` document.
 *
 * Only non-zero counts appear, so an absent id means zero — normal, not an
 * error. A `test <name>` line labels a per-test dump.
 */
fun parseCocoProfile(text: String): CocoProfile {
    val lines = completeLines(text)
    requireHeader(lines, PROFILE_HEADER, "coco-profile")

    var size = 0L
    var label: String? = null
    val hits = HashMap<Long, Long>()

    for (i in 1 until lines.size) {
        val line = lines[i]
        if (line.isEmpty()) continue
        val lineNo = i + 1
        when {
            line.startsWith("size ") ->
                size = line.removePrefix("size ").trim().toLongOrNull()
                    ?: throw CocoFormatException("coco-profile: non-numeric size at line $lineNo")
            line.startsWith("test ") -> label = line.removePrefix("test ").trim()
            else -> {
                val sp = line.indexOf(' ')
                if (sp <= 0) {
                    throw CocoFormatException(
                        "coco-profile: malformed hit row at line $lineNo; expected \"<id> <count>\""
                    )
                }
                val id = line.substring(0, sp).toLongOrNull()
                    ?: throw CocoFormatException("coco-profile: non-numeric probe id at line $lineNo")
                val count = line.substring(sp + 1).trim().toLongOrNull()
                    ?: throw CocoFormatException("coco-profile: non-numeric count at line $lineNo")
                hits[id] = count
            }
        }
    }
    return CocoProfile(size = size, label = label, hits = hits)
}

/**
 * Parse a `coco-attribution v1` document.
 *
 * Its version marker sits **inside a comment**, unlike the other two formats, so
 * the `#` is stripped before matching. Per-test summaries are also comments; the
 * data rows are not.
 */
fun parseCocoAttribution(text: String): CocoAttribution {
    val lines = completeLines(text)
    val header = lines.firstOrNull()?.removePrefix("#")?.trim()
        ?: throw CocoFormatException(
            "coco-attribution: empty document; expected header \"# $ATTRIBUTION_HEADER\""
        )
    if (header != ATTRIBUTION_HEADER) {
        throw CocoFormatException(
            "coco-attribution: unsupported header \"$header\"; " +
                "this build reads only \"$ATTRIBUTION_HEADER\""
        )
    }

    val summaries = ArrayList<CocoTestSummary>()
    val rows = ArrayList<CocoAttributedLine>()

    for (i in 1 until lines.size) {
        val raw = lines[i]
        if (raw.isEmpty()) continue
        val lineNo = i + 1

        if (raw.startsWith("#")) {
            val body = raw.removePrefix("#").trim()
            if (!body.startsWith("test\t") && !body.startsWith("test ")) continue
            val f = body.split('\t')
            if (f.size < 4) continue
            summaries.add(
                CocoTestSummary(
                    name = f[1],
                    covered = f[2].substringAfter("covered=").toLongOrNull() ?: 0L,
                    unique = f[3].substringAfter("unique=").toLongOrNull() ?: 0L,
                )
            )
            continue
        }

        val f = raw.split('\t')
        if (f.size < 4) {
            throw CocoFormatException(
                "coco-attribution: malformed row at line $lineNo; " +
                    "expected 4 tab-separated fields, found ${f.size}"
            )
        }
        val listed = f[3].split('|').filter { it.isNotEmpty() }
        val overflow = listed.lastOrNull()?.takeIf { it.startsWith("+") }
            ?.removePrefix("+")?.toIntOrNull() ?: 0
        rows.add(
            CocoAttributedLine(
                file = f[0],
                line = f[1].toIntOrNull()
                    ?: throw CocoFormatException("coco-attribution: non-numeric line at line $lineNo"),
                testCount = f[2].toIntOrNull() ?: 0,
                tests = if (overflow > 0) listed.dropLast(1) else listed,
                omittedTests = overflow,
            )
        )
    }
    return CocoAttribution(summaries = summaries, lines = rows)
}
