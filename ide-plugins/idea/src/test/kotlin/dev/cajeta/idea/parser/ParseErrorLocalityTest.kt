package dev.cajeta.idea.parser

import dev.cajeta.idea.parser.antlr.CajetaLexer
import dev.cajeta.idea.parser.antlr.CajetaParser
import org.antlr.v4.runtime.BaseErrorListener
import org.antlr.v4.runtime.CharStreams
import org.antlr.v4.runtime.CommonTokenStream
import org.antlr.v4.runtime.RecognitionException
import org.antlr.v4.runtime.Recognizer
import org.antlr.v4.runtime.tree.ErrorNode
import org.antlr.v4.runtime.tree.ParseTree
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * parse-error-locality 2.1.1 (spec §2.2): the plugin's parser, with its
 * anchor-sync strategy, keeps every declaration outside a syntax error.
 */
class ParseErrorLocalityTest {

    private data class Survivors(
        var classes: Int = 0, var methods: Int = 0, var fields: Int = 0,
        var locals: Int = 0, var identifiers: Int = 0, var errorNodes: Int = 0,
    )

    private class Parsed(src: String) {
        val errors = mutableListOf<String>()
        val tokens: CommonTokenStream
        val parser: CajetaParser
        val tree: ParseTree

        init {
            val listener = object : BaseErrorListener() {
                override fun syntaxError(
                    r: Recognizer<*, *>?, sym: Any?, line: Int, col: Int,
                    msg: String?, e: RecognitionException?,
                ) { errors += "$line:$col $msg" }
            }
            val lexer = CajetaLexer(CharStreams.fromString(src)).apply {
                removeErrorListeners(); addErrorListener(listener)
            }
            tokens = CommonTokenStream(lexer)
            parser = CajetaParser(tokens).apply {
                removeErrorListeners(); addErrorListener(listener)
                errorHandler = CajetaErrorStrategy()
            }
            tree = parser.compilationUnit()
        }

        fun consumedEverything() = tokens.index() == tokens.size() - 1

        fun survivors(): Survivors = Survivors().also { walk(tree, it) }

        private fun walk(t: ParseTree, s: Survivors) {
            when (t) {
                is CajetaParser.ClassDeclarationContext -> s.classes++
                is CajetaParser.MethodDeclarationContext -> s.methods++
                is CajetaParser.FieldDeclarationContext -> s.fields++
                is CajetaParser.LocalVariableDeclarationContext -> s.locals++
                is CajetaParser.IdentifierContext -> s.identifiers++
                is ErrorNode -> s.errorNodes++
            }
            for (i in 0 until t.childCount) walk(t.getChild(i), s)
        }
    }

    private fun unit(name: String, second: String) = """
        package demo;

        public final class $name {
            int32 total;

            public int32 first() {
                int32 a = 1;
                return a;
            }

            public int32 second() {
                $second
                return b;
            }
        }
    """.trimIndent() + "\n"

    private val valid = unit("Edit", "int32 b = 2;")
    private val broken = unit("Broken", "int32 b = 2")
    private val paren = unit("Paren", "int32 b = (2;")
    private val quote = unit("Quote", "String s = \"abc;")
    private val two = """
        package demo;

        public final class Alpha {
            public int32 run() {
                int32 x = 1
                return x;
            }
        }

        public final class Beta {
            public int32 go() {
                int32 y = 2;
                return y;
            }
        }
    """.trimIndent() + "\n"

    private fun assertWholeClass(s: Survivors) {
        assertEquals("classes", 1, s.classes)
        assertEquals("methods", 2, s.methods)
        assertEquals("fields", 1, s.fields)
        assertEquals("locals", 2, s.locals)
    }

    @Test
    fun aValidUnitParsesWhole() {
        val p = Parsed(valid)
        assertEquals(emptyList<String>(), p.errors)
        assertTrue(p.consumedEverything())
        val s = p.survivors()
        assertWholeClass(s)
        assertEquals(9, s.identifiers)
    }

    @Test
    fun aMissingSemicolonKeepsEveryOtherDeclaration() {
        val p = Parsed(broken)
        assertEquals(listOf("13:8 missing ';' at 'return'"), p.errors)
        assertTrue(p.consumedEverything())
        val s = p.survivors()
        assertWholeClass(s)
        assertEquals(9, s.identifiers)
    }

    @Test
    fun anUnclosedParenKeepsEveryOtherDeclaration() {
        val p = Parsed(paren)
        assertEquals(1, p.errors.size)
        assertTrue(p.consumedEverything())
        val s = p.survivors()
        assertWholeClass(s)
        assertEquals(9, s.identifiers)
    }

    @Test
    fun anUnterminatedStringKeepsEveryOtherDeclaration() {
        val p = Parsed(quote)
        assertTrue(p.errors.isNotEmpty())
        assertTrue(p.consumedEverything())
        val s = p.survivors()
        assertWholeClass(s)
        assertTrue("identifiers ${s.identifiers}", s.identifiers >= 9)
    }

    @Test
    fun aBrokenFirstClassLeavesTheSecondWhole() {
        val p = Parsed(two)
        assertEquals(1, p.errors.size)
        assertTrue(p.consumedEverything())
        val s = p.survivors()
        assertEquals(2, s.classes)
        assertEquals(2, s.methods)
        assertEquals(9, s.identifiers)
    }

    @Test
    fun noSyntaxErrorQuotesTheFileFromItsFirstToken() {
        for (src in listOf(broken, paren, quote, two)) {
            for (e in Parsed(src).errors) {
                assertTrue(e, !e.contains("no viable alternative at input 'package"))
                assertTrue(e, !e.startsWith("1:"))
            }
        }
    }
}
