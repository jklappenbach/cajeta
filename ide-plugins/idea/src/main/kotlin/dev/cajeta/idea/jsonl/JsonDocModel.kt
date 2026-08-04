package dev.cajeta.idea.jsonl

import dev.cajeta.idea.debugger.Json

/** Half-open offset range into the source text of a parsed document. */
data class JsonSpan(val start: Int, val end: Int)

/**
 * The document model shared by every json-viewer surface (spec §2.1.1): a JSON
 * value tree whose nodes carry source spans, so the structured editor can map a
 * cell back to the exact text it came from. Values reuse the bundled [Json] DOM
 * ([toJson]); the node layer only adds spans and key ordering.
 */
sealed class JsonDocNode {
    abstract val span: JsonSpan

    data class Scalar(override val span: JsonSpan, val value: Json) : JsonDocNode()

    data class ArrayNode(
        override val span: JsonSpan,
        val elements: List<JsonDocNode>,
    ) : JsonDocNode()

    data class ObjectNode(
        override val span: JsonSpan,
        val entries: List<Entry>,
    ) : JsonDocNode() {
        data class Entry(val key: String, val keySpan: JsonSpan, val value: JsonDocNode)
    }

    fun toJson(): Json = when (this) {
        is Scalar -> value
        is ArrayNode -> Json.Arr(elements.mapTo(mutableListOf()) { it.toJson() })
        is ObjectNode -> Json.Obj(entries.associateTo(LinkedHashMap()) { it.key to it.value.toJson() })
    }
}

/** All-or-nothing parse outcome (spec §2.1.2): a tree, or a located error — never both. */
sealed class JsonDocResult {
    data class Ok(val root: JsonDocNode) : JsonDocResult()
    data class Err(val line: Int, val column: Int, val message: String) : JsonDocResult()
}

/**
 * Span-carrying document parser. Lenient mode (the default, spec §2.1.4) accepts
 * JSONC: `//` and `/* */` comments and trailing commas; strict mode rejects them,
 * which is what makes the engine's strict-first line classification meaningful.
 */
object JsonDocModel {

    fun parse(text: String, lenient: Boolean = true): JsonDocResult = try {
        JsonDocResult.Ok(Parser(text, lenient).parseTopLevel())
    } catch (e: ParseError) {
        val (line, column) = lineColOf(text, e.offset)
        JsonDocResult.Err(line, column, e.message ?: "parse error")
    }

    private fun lineColOf(text: String, offset: Int): Pair<Int, Int> {
        var line = 1
        var col = 1
        for (i in 0 until offset.coerceAtMost(text.length)) {
            if (text[i] == '\n') { line++; col = 1 } else col++
        }
        return line to col
    }

    private class ParseError(val offset: Int, message: String) : Exception(message)

    private class Parser(private val s: String, private val lenient: Boolean) {
        private var i = 0

        fun parseTopLevel(): JsonDocNode {
            val v = parseValue()
            skipWs()
            if (i < s.length) fail("trailing characters")
            return v
        }

        private fun parseValue(): JsonDocNode {
            skipWs()
            if (i >= s.length) fail("unexpected end of input")
            return when (val c = s[i]) {
                '{' -> parseObject()
                '[' -> parseArray()
                '"' -> {
                    val start = i
                    val str = parseString()
                    JsonDocNode.Scalar(JsonSpan(start, i), Json.Str(str))
                }
                't', 'f', 'n' -> parseLiteral()
                else ->
                    if (c == '-' || c in '0'..'9') parseNumber()
                    else fail("unexpected '$c'")
            }
        }

        private fun parseObject(): JsonDocNode.ObjectNode {
            val start = i
            expect('{')
            val entries = ArrayList<JsonDocNode.ObjectNode.Entry>()
            skipWs()
            if (peekIs('}')) { i++; return JsonDocNode.ObjectNode(JsonSpan(start, i), entries) }
            while (true) {
                skipWs()
                val keyStart = i
                val key = parseString()
                val keySpan = JsonSpan(keyStart, i)
                skipWs()
                expect(':')
                entries += JsonDocNode.ObjectNode.Entry(key, keySpan, parseValue())
                skipWs()
                when (val c = next()) {
                    ',' -> {
                        skipWs()
                        if (peekIs('}')) {          // trailing comma
                            if (!lenient) fail("trailing comma")
                            i++
                            break
                        }
                    }
                    '}' -> break
                    else -> fail("expected ',' or '}' but got '$c'", i - 1)
                }
            }
            return JsonDocNode.ObjectNode(JsonSpan(start, i), entries)
        }

        private fun parseArray(): JsonDocNode.ArrayNode {
            val start = i
            expect('[')
            val elements = ArrayList<JsonDocNode>()
            skipWs()
            if (peekIs(']')) { i++; return JsonDocNode.ArrayNode(JsonSpan(start, i), elements) }
            while (true) {
                elements += parseValue()
                skipWs()
                when (val c = next()) {
                    ',' -> {
                        skipWs()
                        if (peekIs(']')) {          // trailing comma
                            if (!lenient) fail("trailing comma")
                            i++
                            break
                        }
                    }
                    ']' -> break
                    else -> fail("expected ',' or ']' but got '$c'", i - 1)
                }
            }
            return JsonDocNode.ArrayNode(JsonSpan(start, i), elements)
        }

        private fun parseString(): String {
            if (!peekIs('"')) fail("expected string")
            i++
            val sb = StringBuilder()
            while (true) {
                if (i >= s.length) fail("unterminated string")
                when (val c = s[i++]) {
                    '"' -> return sb.toString()
                    '\\' -> {
                        if (i >= s.length) fail("unterminated escape")
                        when (val e = s[i++]) {
                            '"' -> sb.append('"')
                            '\\' -> sb.append('\\')
                            '/' -> sb.append('/')
                            'b' -> sb.append('\b')
                            'f' -> sb.append('\u000C')
                            'n' -> sb.append('\n')
                            'r' -> sb.append('\r')
                            't' -> sb.append('\t')
                            'u' -> {
                                if (i + 4 > s.length) fail("truncated \\u escape")
                                sb.append(s.substring(i, i + 4).toInt(16).toChar())
                                i += 4
                            }
                            else -> fail("bad escape '\\$e'", i - 1)
                        }
                    }
                    else -> sb.append(c)
                }
            }
        }

        private fun parseNumber(): JsonDocNode.Scalar {
            val start = i
            if (peekIs('-')) i++
            while (i < s.length && s[i] in '0'..'9') i++
            var isInt = true
            if (i < s.length && s[i] == '.') {
                isInt = false
                i++
                while (i < s.length && s[i] in '0'..'9') i++
            }
            if (i < s.length && (s[i] == 'e' || s[i] == 'E')) {
                isInt = false
                i++
                if (i < s.length && (s[i] == '+' || s[i] == '-')) i++
                while (i < s.length && s[i] in '0'..'9') i++
            }
            val token = s.substring(start, i)
            val value = token.toDoubleOrNull() ?: fail("bad number '$token'", start)
            return JsonDocNode.Scalar(JsonSpan(start, i), Json.Num(value, isInt))
        }

        private fun parseLiteral(): JsonDocNode.Scalar {
            val start = i
            return when {
                s.startsWith("true", i) -> { i += 4; JsonDocNode.Scalar(JsonSpan(start, i), Json.Bool(true)) }
                s.startsWith("false", i) -> { i += 5; JsonDocNode.Scalar(JsonSpan(start, i), Json.Bool(false)) }
                s.startsWith("null", i) -> { i += 4; JsonDocNode.Scalar(JsonSpan(start, i), Json.Null) }
                else -> fail("invalid literal")
            }
        }

        /** Whitespace, plus `//` and `/* */` comments in lenient mode. */
        private fun skipWs() {
            while (i < s.length) {
                val c = s[i]
                when {
                    c.isWhitespace() -> i++
                    lenient && c == '/' && i + 1 < s.length && s[i + 1] == '/' -> {
                        while (i < s.length && s[i] != '\n') i++
                    }
                    lenient && c == '/' && i + 1 < s.length && s[i + 1] == '*' -> {
                        val close = s.indexOf("*/", i + 2)
                        if (close < 0) fail("unterminated comment")
                        i = close + 2
                    }
                    else -> return
                }
            }
        }

        private fun peekIs(c: Char): Boolean = i < s.length && s[i] == c
        private fun next(): Char {
            if (i >= s.length) fail("unexpected end of input")
            return s[i++]
        }
        private fun expect(c: Char) {
            if (!peekIs(c)) fail("expected '$c'")
            i++
        }
        private fun fail(message: String, at: Int = i): Nothing = throw ParseError(at, message)
    }
}
