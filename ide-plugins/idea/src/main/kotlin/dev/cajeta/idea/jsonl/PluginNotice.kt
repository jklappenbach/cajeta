package dev.cajeta.idea.jsonl

import dev.cajeta.idea.debugger.Json

/**
 * The plugin's own console lines, in the compiler's stream format.
 *
 * The debug console renders one stream containing two writers: the compiler
 * (every line a record since compiler-jsonl Unit 1) and the plugin itself —
 * "breakpoint not set", and anything else the IDE needs to say about a run. A
 * prose line among records is not merely inconsistent: it sits outside every
 * level filter and carries no tint, so the one notice a developer most needs
 * is the one most easily lost. Julian, 2026-07-31: "one line not in jsonl".
 *
 * These are `log` records rather than a plugin-specific kind. The kind is
 * already defined for exactly this — levelled narration carried verbatim — and
 * inventing one here would put a record on the wire that the compiler's schema
 * guard (which scans `src/`) could never see, so it would document nothing and
 * rot unwatched.
 *
 * Built through the [Json] DOM, never string concatenation: a message
 * containing a quote or a backslash would otherwise emit a line the console
 * then fails to parse, turning a warning into a silently dropped row.
 */
object PluginNotice {

    /** One `log` record, newline-terminated, ready to write to the console. */
    fun log(level: String, message: String): String =
        Json.obj(
            "kind" to Json.of("log"),
            "level" to Json.of(level),
            "message" to Json.of(message),
        ).toCompactString() + "\n"
}
