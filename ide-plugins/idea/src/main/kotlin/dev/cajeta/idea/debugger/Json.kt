package dev.cajeta.idea.debugger

/**
 * A minimal JSON DOM + parser/serializer, mirroring the compiler's
 * hand-written `src/cajeta/dap/Json.{h,cpp}` so the IDE client and the
 * `cajeta dap` server speak byte-for-byte the same dialect. Deliberately
 * dependency-free (no Gson, no kotlinx.serialization, no IntelliJ platform
 * classes) so the DAP transport/protocol layer is unit-testable as plain
 * JVM code without booting a platform test fixture.
 *
 * Numbers are carried as Double with an `isInt` flag so integers serialize
 * without a trailing `.0`. DAP payloads stay well within Double's exact
 * integer range; large 64-bit values (e.g. pointers) arrive inside strings
 * (`<Type@0x...>`), never as bare numbers.
 */
sealed class Json {

    object Null : Json()
    data class Bool(val value: Boolean) : Json()
    data class Num(val value: Double, val isInt: Boolean) : Json()
    data class Str(val value: String) : Json()
    data class Arr(val items: MutableList<Json> = mutableListOf()) : Json()
    data class Obj(val entries: LinkedHashMap<String, Json> = LinkedHashMap()) : Json()

    // ---- typed accessors (throw on the wrong shape, like the C++ at()) ----

    fun asString(): String = (this as Str).value
    fun asInt(): Int = (this as Num).value.toInt()
    fun asLong(): Long = (this as Num).value.toLong()
    fun asDouble(): Double = (this as Num).value
    fun asBool(): Boolean = (this as Bool).value

    /** Member by key, or null if this isn't an object or the key is absent. */
    fun opt(key: String): Json? = (this as? Obj)?.entries?.get(key)

    /** Member by key; throws if missing (mirrors C++ `at`). */
    fun at(key: String): Json =
        opt(key) ?: throw NoSuchElementException("no JSON member '$key'")

    /** Array element by index. */
    operator fun get(index: Int): Json = (this as Arr).items[index]

    /** Element count for arrays/objects; 0 otherwise. */
    val size: Int
        get() = when (this) {
            is Arr -> items.size
            is Obj -> entries.size
            else -> 0
        }

    fun add(item: Json): Json {
        (this as Arr).items.add(item)
        return this
    }

    operator fun set(key: String, value: Json) {
        (this as Obj).entries[key] = value
    }

    // ---- serialization ----

    fun toCompactString(): String = buildString { writeTo(this) }

    private fun writeTo(sb: StringBuilder) {
        when (this) {
            is Null -> sb.append("null")
            is Bool -> sb.append(if (value) "true" else "false")
            is Num ->
                if (isInt) sb.append(value.toLong().toString())
                else sb.append(value.toString())
            is Str -> writeString(sb, value)
            is Arr -> {
                sb.append('[')
                items.forEachIndexed { i, item ->
                    if (i > 0) sb.append(',')
                    item.writeTo(sb)
                }
                sb.append(']')
            }
            is Obj -> {
                sb.append('{')
                var first = true
                for ((k, v) in entries) {
                    if (!first) sb.append(',')
                    first = false
                    writeString(sb, k)
                    sb.append(':')
                    v.writeTo(sb)
                }
                sb.append('}')
            }
        }
    }

    companion object {
        fun of(v: String): Json = Str(v)
        fun of(v: Int): Json = Num(v.toDouble(), true)
        fun of(v: Long): Json = Num(v.toDouble(), true)
        fun of(v: Double): Json = Num(v, false)
        fun of(v: Boolean): Json = Bool(v)

        fun obj(vararg pairs: Pair<String, Json>): Obj {
            val o = Obj()
            for ((k, v) in pairs) o.entries[k] = v
            return o
        }

        fun arr(vararg items: Json): Arr = Arr(items.toMutableList())

        fun parse(text: String): Json = Parser(text).parseTopLevel()

        private fun writeString(sb: StringBuilder, s: String) {
            sb.append('"')
            for (c in s) {
                when (c) {
                    '"' -> sb.append("\\\"")
                    '\\' -> sb.append("\\\\")
                    '\b' -> sb.append("\\b")
                    '\n' -> sb.append("\\n")
                    '\r' -> sb.append("\\r")
                    '\t' -> sb.append("\\t")
                    else ->
                        // Control chars (incl. form feed U+000C) emit as \uXXXX.
                        if (c < ' ') sb.append("\\u%04x".format(c.code))
                        else sb.append(c)
                }
            }
            sb.append('"')
        }
    }

    /** Recursive-descent parser; throws IllegalArgumentException on malformed input. */
    private class Parser(private val s: String) {
        private var i = 0

        fun parseTopLevel(): Json {
            skipWs()
            val v = parseValue()
            skipWs()
            require(i >= s.length) { "trailing characters at offset $i" }
            return v
        }

        private fun parseValue(): Json {
            skipWs()
            require(i < s.length) { "unexpected end of input" }
            return when (val c = s[i]) {
                '{' -> parseObject()
                '[' -> parseArray()
                '"' -> Str(parseString())
                't', 'f' -> parseBool()
                'n' -> parseNull()
                else ->
                    if (c == '-' || c in '0'..'9') parseNumber()
                    else throw IllegalArgumentException("unexpected '$c' at offset $i")
            }
        }

        private fun parseObject(): Obj {
            expect('{')
            val o = Obj()
            skipWs()
            if (peek() == '}') { i++; return o }
            while (true) {
                skipWs()
                val key = parseString()
                skipWs()
                expect(':')
                o.entries[key] = parseValue()
                skipWs()
                when (val c = next()) {
                    ',' -> continue
                    '}' -> break
                    else -> throw IllegalArgumentException("expected ',' or '}' but got '$c'")
                }
            }
            return o
        }

        private fun parseArray(): Arr {
            expect('[')
            val a = Arr()
            skipWs()
            if (peek() == ']') { i++; return a }
            while (true) {
                a.items.add(parseValue())
                skipWs()
                when (val c = next()) {
                    ',' -> continue
                    ']' -> break
                    else -> throw IllegalArgumentException("expected ',' or ']' but got '$c'")
                }
            }
            return a
        }

        private fun parseString(): String {
            expect('"')
            val sb = StringBuilder()
            while (true) {
                require(i < s.length) { "unterminated string" }
                val c = s[i++]
                when (c) {
                    '"' -> return sb.toString()
                    '\\' -> {
                        require(i < s.length) { "unterminated escape" }
                        when (val e = s[i++]) {
                            '"' -> sb.append('"')
                            '\\' -> sb.append('\\')
                            '/' -> sb.append('/')
                            'b' -> sb.append('\b')
                            'f' -> sb.append('')
                            'n' -> sb.append('\n')
                            'r' -> sb.append('\r')
                            't' -> sb.append('\t')
                            'u' -> {
                                require(i + 4 <= s.length) { "truncated \\u escape" }
                                val hex = s.substring(i, i + 4)
                                i += 4
                                sb.append(hex.toInt(16).toChar())
                            }
                            else -> throw IllegalArgumentException("bad escape '\\$e'")
                        }
                    }
                    else -> sb.append(c)
                }
            }
        }

        private fun parseNumber(): Num {
            val start = i
            if (peek() == '-') i++
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
            return Num(token.toDouble(), isInt)
        }

        private fun parseBool(): Bool =
            when {
                s.startsWith("true", i) -> { i += 4; Bool(true) }
                s.startsWith("false", i) -> { i += 5; Bool(false) }
                else -> throw IllegalArgumentException("invalid literal at offset $i")
            }

        private fun parseNull(): Json =
            if (s.startsWith("null", i)) { i += 4; Null }
            else throw IllegalArgumentException("invalid literal at offset $i")

        private fun skipWs() {
            while (i < s.length && s[i].isWhitespace()) i++
        }

        private fun peek(): Char = if (i < s.length) s[i] else ' '
        private fun next(): Char {
            require(i < s.length) { "unexpected end of input" }
            return s[i++]
        }
        private fun expect(c: Char) {
            require(i < s.length && s[i] == c) { "expected '$c' at offset $i" }
            i++
        }
    }
}
