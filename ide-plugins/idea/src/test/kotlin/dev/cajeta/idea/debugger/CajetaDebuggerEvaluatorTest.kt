package dev.cajeta.idea.debugger

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * debugger-variable-inspection Unit 6 (spec §7), plugin half: what the hover
 * asks for, and what it makes of the answer. Both halves are pure — the
 * XDebuggerEvaluator itself is a thin adapter over these.
 */
class CajetaDebuggerEvaluatorTest {

    // --- 6.1.5a what gets asked (§7.1.2) -----------------------------------

    private fun exprAt(text: String, offset: Int): String? =
        CajetaHoverExpression.at(text, offset)?.expression

    @Test
    fun aBareIdentifierIsPickedUp() {
        val line = "    int32 total = count + 1;"
        assertEquals("total", exprAt(line, line.indexOf("total")))
        assertEquals("count", exprAt(line, line.indexOf("count") + 2))
    }

    @Test
    fun aDottedPathIsPickedUpWhole() {
        // §7.2.2: hovering anywhere in `origin.x` must ask for `origin.x`.
        // The platform default would pick the bare word `x`, which is not a
        // frame local — the popup would say "not available" for a value the
        // server can resolve perfectly well.
        val line = "        return origin.x + origin.y;"
        assertEquals("origin.x", exprAt(line, line.indexOf("origin.x")))
        assertEquals("origin.x", exprAt(line, line.indexOf("origin.x") + 7))
        assertEquals("origin.y", exprAt(line, line.indexOf("origin.y") + 3))
    }

    @Test
    fun anIndexedPathIsPickedUpWhole() {
        val line = "        int32 first = nums[0];"
        assertEquals("nums[0]", exprAt(line, line.indexOf("nums")))
        assertEquals("nums[0]", exprAt(line, line.indexOf("[0]") + 1))
    }

    @Test
    fun aDeepPathIsPickedUpWhole() {
        val line = "  x = shapes[2].origin.x;"
        assertEquals("shapes[2].origin.x", exprAt(line, line.indexOf("shapes")))
    }

    @Test
    fun hoveringTheEndOfAnIdentifierStillResolves() {
        // Where the caret lands when you hover the last character.
        val line = "return total;"
        assertEquals("total", exprAt(line, line.indexOf("total") + 5))
    }

    @Test
    fun nonPathsAreNotAskedAbout() {
        // Whitespace, punctuation and keawords-with-calls resolve to nothing
        // rather than to a request the server would have to reject (§7.2.3).
        assertNull(exprAt("        return a + b;", 0))
        assertNull(exprAt("  x = 42;", "  x = ".length))          // a literal
        assertNull(exprAt("", 0))
        assertNull(exprAt("a", 5))                                 // past the end
    }

    @Test
    fun theGrammarMatchesWhatTheServerAccepts() {
        // Anything true here must be resolvable by the server's parseSimplePath;
        // anything false must never be sent.
        for (good in listOf("a", "abc", "_x", "a.b", "a.b.c", "arr[0]", "arr[12]",
                            "a[0].b", "shapes[2].origin.x", "x1.y2")) {
            assertTrue("$good should be a simple path", CajetaHoverExpression.isSimplePath(good))
        }
        for (bad in listOf("", "1", "1a", ".a", "a.", "a..b", "a[", "a[]", "a[x]",
                           "a[0", "a b", "f()", "a+b", "a->b", "a[-1]")) {
            assertFalse("$bad must not be sent", CajetaHoverExpression.isSimplePath(bad))
        }
    }

    // --- 6.1.5b what is made of the answer (§7.1.1, §7.2.3, §7.2.4) --------

    private fun response(body: String): Json = Json.parse(body)

    @Test
    fun aSuccessfulEvaluateBecomesAShowableValue() {
        val out = parseEvaluate(response(
            """{"type":"response","success":true,"body":
               {"result":"{3 elements}","type":"int32[]","variablesReference":7}}"""), "nums")
        assertTrue(out.resolved)
        assertEquals("{3 elements}", out.value!!.value)
        assertEquals("int32[]", out.value!!.type)
        assertEquals(7, out.value!!.variablesReference)
        assertEquals("nums", out.value!!.name)
        assertNull(out.message)
    }

    @Test
    fun aLeafCarriesNoExpansionReference() {
        val out = parseEvaluate(response(
            """{"type":"response","success":true,"body":
               {"result":"3","type":"int32","variablesReference":0}}"""), "origin.x")
        assertTrue(out.resolved)
        assertEquals(0, out.value!!.variablesReference)
    }

    @Test
    fun anUnresolvedIdentifierExplainsItselfInsteadOfErroring() {
        // §7.2.4: the popup should say nothing much, not raise a dialog.
        val out = parseEvaluate(response(
            """{"type":"response","success":false,"message":"not available: ghost"}"""), "ghost")
        assertFalse(out.resolved)
        assertEquals("not available: ghost", out.message)
    }

    @Test
    fun anUnsupportedExpressionSaysSoPlainly() {
        // §7.2.3: distinguishable from "not available", so the popup can tell
        // the reader which of the two happened.
        val out = parseEvaluate(response(
            """{"type":"response","success":false,"message":"unsupported expression: f()"}"""), "f()")
        assertFalse(out.resolved)
        assertTrue(out.message!!.startsWith("unsupported"))
    }

    @Test
    fun aMalformedResponseIsStillAnOutcome() {
        // Never an exception on the hover path.
        assertFalse(parseEvaluate(response("""{"type":"response","success":true}""")).resolved)
        assertFalse(parseEvaluate(response("""{"success":true,"body":{}}""")).resolved)
        assertFalse(parseEvaluate(response("""{}""")).resolved)
        assertEquals("not available", parseEvaluate(response("""{}""")).message)
    }
}
