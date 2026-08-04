package dev.cajeta.idea.usages

import dev.cajeta.idea.debugger.Json

/**
 * The pure half of find usages (ide-features spec §2.0.4): reading the
 * compiler's reference records and classifying them.
 *
 * The index already holds this relation — `uses:<fqn>` is the find-usages
 * direction, written by the same export that powers Ctrl-click — so nothing
 * here searches. It converts what the compiler already resolved into
 * something the platform's usage view can show, which is why overloads are
 * never conflated (§2.1.3) and usages in dependency or stdlib source are
 * found like any other (§2.1.4): the compiler resolved them all.
 */
object CajetaUsageRecords {

    /** One use site, exactly as the compiler exported it. */
    data class UseSite(
        val file: String,
        val line: Int,
        val col: Int,
        val kind: CajetaUsageKind,
        /** The declaration the reference was written inside, when exported. */
        val from: String?,
    )

    /** Read a reference record; null when it lacks a usable position. */
    fun parse(record: Json.Obj): UseSite? {
        val file = (record.entries["file"] as? Json.Str)?.value ?: return null
        if (file.isBlank()) return null
        val line = (record.entries["line"] as? Json.Num)?.value?.toInt() ?: return null
        // A line of 0 is the export saying "no position", not line zero.
        if (line <= 0) return null
        val col = (record.entries["col"] as? Json.Num)?.value?.toInt() ?: 0
        return UseSite(
            file = file,
            line = line,
            col = maxOf(0, col),
            kind = CajetaUsageKind.of((record.entries["kind"] as? Json.Str)?.value),
            from = (record.entries["from"] as? Json.Str)?.value?.ifBlank { null },
        )
    }

    fun parseAll(records: List<Json.Obj>): List<UseSite> = records.mapNotNull(::parse)

    /**
     * Group use sites by file, preserving each file's first-seen order and
     * ordering the sites within a file by position. The usage view groups by
     * file itself, but a stable order is what keeps the tree from reshuffling
     * between invocations of the same search.
     */
    fun byFile(sites: List<UseSite>): LinkedHashMap<String, List<UseSite>> {
        val out = LinkedHashMap<String, MutableList<UseSite>>()
        for (s in sites) out.getOrPut(s.file) { mutableListOf() } += s
        val ordered = LinkedHashMap<String, List<UseSite>>()
        for ((file, group) in out) {
            ordered[file] = group.sortedWith(compareBy({ it.line }, { it.col }))
        }
        return ordered
    }
}

/**
 * How a reference uses its target — the grouping the usage view shows
 * (§2.1.2). The strings are the compiler's `kind` field; anything
 * unrecognized is [OTHER] rather than a guess, so a new export kind shows up
 * as an ungrouped usage instead of being silently mislabelled.
 */
enum class CajetaUsageKind {
    READ, WRITE, CALL, INHERIT, IMPORT, TYPE, OTHER;

    companion object {
        fun of(raw: String?): CajetaUsageKind = when (raw?.lowercase()) {
            "read", "load", "get" -> READ
            "write", "store", "set", "assign" -> WRITE
            "call", "invoke", "new", "ctor" -> CALL
            "inherit", "extends", "implements", "override" -> INHERIT
            "import" -> IMPORT
            "type", "typeref", "cast", "annotation" -> TYPE
            else -> OTHER
        }
    }
}
