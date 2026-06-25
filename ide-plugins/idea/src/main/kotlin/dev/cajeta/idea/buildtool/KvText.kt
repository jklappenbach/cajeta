package dev.cajeta.idea.buildtool

/**
 * Parses newline-delimited `key=value` text (how a run config persists its `-P`
 * properties and `-p` params) into an ordered map. Pure; blank lines and lines
 * without `=` are ignored, keys/values trimmed. The inverse [format] keeps the
 * editor field and the persisted string in sync.
 */
object KvText {
    fun parse(text: String): Map<String, String> {
        val out = LinkedHashMap<String, String>()
        for (line in text.lineSequence()) {
            val t = line.trim()
            if (t.isEmpty()) continue
            val eq = t.indexOf('=')
            if (eq <= 0) continue
            out[t.substring(0, eq).trim()] = t.substring(eq + 1).trim()
        }
        return out
    }

    fun format(map: Map<String, String>): String =
        map.entries.joinToString("\n") { "${it.key}=${it.value}" }
}
