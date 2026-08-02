package dev.cajeta.idea.debugger

/**
 * Which text under the cursor is worth asking the debugger about
 * (debugger-variable-inspection spec §7.1.2, §7.2.2).
 *
 * The platform's default hover picks the bare word at the offset, which would
 * turn a hover over `origin.x` into a request for `x` — an identifier that
 * isn't in the frame, so the popup would say "not available" for something the
 * server can actually resolve. This walks the same simple path grammar the
 * SERVER accepts (`name`, `a.b.c`, `arr[0]`, and combinations) and nothing
 * more: matching the two ends means a hover never asks a question the server
 * has to reject.
 *
 * Pure string work, so the grammar is testable without an editor.
 */
object CajetaHoverExpression {

    /** The path expression surrounding [offset], or null if there isn't one. */
    fun at(text: CharSequence, offset: Int): TextRangeSpan? {
        if (offset < 0 || offset > text.length) return null
        // An offset sitting just past the end of an identifier still counts —
        // that is where the caret lands when you hover the last character.
        var probe = offset
        if (probe == text.length || !isPathChar(text[probe])) {
            if (probe > 0 && isPathChar(text[probe - 1])) probe-- else return null
        }
        if (!isIdentPart(text[probe]) && text[probe] != '[' && text[probe] != ']') return null

        var start = probe
        while (start > 0 && isPathChar(text[start - 1])) start--
        // A path starts at an identifier, never at '.', '[' or a digit.
        while (start < text.length && !isIdentStart(text[start])) start++
        if (start > probe) return null

        var end = probe + 1
        while (end < text.length && isPathChar(text[end])) end++
        // ...and ends at an identifier char or a closing bracket, so a trailing
        // '.' or half-open '[' is not sent.
        while (end > start && !isIdentPart(text[end - 1]) && text[end - 1] != ']') end--
        if (end <= start) return null

        val expr = text.subSequence(start, end).toString()
        if (!isSimplePath(expr)) return null
        return TextRangeSpan(start, end, expr)
    }

    /**
     * The grammar the server's `parseSimplePath` accepts: an identifier root
     * followed by any number of `.field` and `[integer]` steps. Anything with
     * a call, an operator or whitespace is rejected HERE, so the popup never
     * shows the server's "unsupported expression" for something we could have
     * known not to ask.
     */
    fun isSimplePath(expr: String): Boolean {
        if (expr.isEmpty()) return false
        var i = 0
        if (!isIdentStart(expr[i])) return false
        while (i < expr.length && isIdentPart(expr[i])) i++
        while (i < expr.length) {
            when (expr[i]) {
                '.' -> {
                    i++
                    if (i >= expr.length || !isIdentStart(expr[i])) return false
                    while (i < expr.length && isIdentPart(expr[i])) i++
                }
                '[' -> {
                    i++
                    val digitsFrom = i
                    while (i < expr.length && expr[i].isDigit()) i++
                    if (i == digitsFrom) return false          // `[]` or `[x]`
                    if (i >= expr.length || expr[i] != ']') return false
                    i++
                }
                else -> return false
            }
        }
        return true
    }

    private fun isIdentStart(c: Char): Boolean = c.isLetter() || c == '_'
    private fun isIdentPart(c: Char): Boolean = c.isLetterOrDigit() || c == '_'
    private fun isPathChar(c: Char): Boolean =
        isIdentPart(c) || c == '.' || c == '[' || c == ']'

    /** A resolved span plus the text it covers. */
    data class TextRangeSpan(val start: Int, val end: Int, val expression: String)
}
