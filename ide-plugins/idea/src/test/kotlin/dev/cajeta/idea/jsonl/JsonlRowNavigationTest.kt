package dev.cajeta.idea.jsonl

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * json-viewer unit 3 (spec §3.1.6, §3.2.3): structured rows carrying source
 * coordinates — the compiler's NDJSON diagnostic shape (file/line/column,
 * 1-based) — yield a location; rows without coordinates yield none. Path
 * resolution is pure (filesystem injected): absolute-if-exists, else against
 * the roots in order, unresolvable = not clickable.
 */
class JsonlRowNavigationTest {

    private fun rec(json: String) = JsonlEngine.parseLine(1, json) as JsonlRow.Record

    @Test
    fun compilerDiagnosticRowYieldsFileLineColumn() {
        val row = rec("""{"severity":"error","message":"bad","file":"src/Main.cajeta","line":12,"column":5}""")
        assertEquals(RowLocation("src/Main.cajeta", 12, 5), JsonlRowNavigation.locationOf(row))
    }

    @Test
    fun missingLineOrColumnDefaultToOne() {
        val row = rec("""{"level":"warn","message":"m","file":"/abs/T.cajeta"}""")
        assertEquals(RowLocation("/abs/T.cajeta", 1, 1), JsonlRowNavigation.locationOf(row))
        val lineOnly = rec("""{"message":"m","file":"a.cajeta","line":7}""")
        assertEquals(RowLocation("a.cajeta", 7, 1), JsonlRowNavigation.locationOf(lineOnly))
    }

    @Test
    fun rowsWithoutCoordinatesYieldNone() {
        assertNull(JsonlRowNavigation.locationOf(rec("""{"level":"info","message":"no location"}""")))
        assertNull(JsonlRowNavigation.locationOf(JsonlRow.Raw(1, "plain text")))
        // a non-string file or wrong-typed line is not a location
        assertNull(JsonlRowNavigation.locationOf(rec("""{"message":"m","file":42}""")))
    }

    @Test
    fun absolutePathResolvesOnlyIfItExists() {
        val loc = RowLocation("/proj/src/Main.cajeta", 1, 1)
        assertEquals(
            "/proj/src/Main.cajeta",
            JsonlRowNavigation.resolve(loc, emptyList()) { it == "/proj/src/Main.cajeta" },
        )
        assertNull(JsonlRowNavigation.resolve(loc, listOf("/other")) { false })
    }

    @Test
    fun relativePathResolvesAgainstRootsInOrder() {
        val loc = RowLocation("src/Main.cajeta", 3, 1)
        val existing = setOf("/second/src/Main.cajeta", "/third/src/Main.cajeta")
        assertEquals(
            "/second/src/Main.cajeta",
            JsonlRowNavigation.resolve(loc, listOf("/first", "/second", "/third")) { it in existing },
        )
        assertNull(JsonlRowNavigation.resolve(loc, listOf("/first")) { it in existing })
    }
}
