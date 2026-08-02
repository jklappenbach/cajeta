package dev.cajeta.idea.jsonl

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * json-viewer Unit 8 (spec §3.1.9): the table layout a reader set up — which
 * columns, in what order, at what width — survives into the next session of
 * the same run/debug profile. Pure: the codec and the model decide everything;
 * the platform service only stores the string.
 */
class JsonlColumnLayoutTest {

    private fun record(line: Int, json: String): JsonlRow =
        JsonlEngine.parseLine(line, json)!!

    // --- 8.1.1 round-trip ---------------------------------------------------

    @Test
    fun layoutRoundTripsThroughItsCodec() {
        val layout = JsonlColumnLayout(
            columns = listOf("level", "message", "file"),
            widths = mapOf("message" to 620, "file" to 180),
        )
        val decoded = JsonlColumnLayout.decode(layout.encode())

        assertEquals(layout.columns, decoded?.columns)
        assertEquals(layout.widths, decoded?.widths)
    }

    @Test
    fun columnOrderIsPreservedNotSorted() {
        val layout = JsonlColumnLayout(listOf("zeta", "alpha", "middle"), emptyMap())
        assertEquals(listOf("zeta", "alpha", "middle"), JsonlColumnLayout.decode(layout.encode())?.columns)
    }

    @Test
    fun awkwardFieldNamesSurvive() {
        // Field names come from arbitrary JSON keys — a codec that assumed
        // `name:width|name:width` would lose every one of these.
        val names = listOf("""quote"inside""", "with|pipe", "with:colon", "ünïcodé", "sp ace")
        val layout = JsonlColumnLayout(names, names.associateWith { 100 })
        val decoded = JsonlColumnLayout.decode(layout.encode())

        assertEquals(names, decoded?.columns)
        assertEquals(names.associateWith { 100 }, decoded?.widths)
    }

    @Test
    fun anEmptySelectionRoundTrips() {
        // §3.1.7.3 makes "no columns" a legal, deliberate state; it must
        // persist as itself and not decode back to the defaults.
        val decoded = JsonlColumnLayout.decode(JsonlColumnLayout(emptyList(), emptyMap()).encode())
        assertEquals(emptyList<String>(), decoded?.columns)
    }

    // --- 8.1.2 bad input ----------------------------------------------------

    @Test
    fun unreadablePayloadsDecodeToNullRatherThanThrowing() {
        // §3.1.9.5: layout is a convenience; it never blocks a console.
        assertNull(JsonlColumnLayout.decode(null))
        assertNull(JsonlColumnLayout.decode(""))
        assertNull(JsonlColumnLayout.decode("   "))
        assertNull(JsonlColumnLayout.decode("not json at all"))
        assertNull(JsonlColumnLayout.decode("""{"columns":"not-an-array"}"""))
        assertNull(JsonlColumnLayout.decode("""[1,2,3]"""))
        assertNull(JsonlColumnLayout.decode("""{"widths":{"a":1}}"""), )
    }

    @Test
    fun nonNumericOrNegativeWidthsAreDroppedNotFatal() {
        val decoded = JsonlColumnLayout.decode(
            """{"columns":["a","b","c"],"widths":{"a":"wide","b":-4,"c":120}}""")
        assertEquals(listOf("a", "b", "c"), decoded?.columns)
        assertEquals(mapOf("c" to 120), decoded?.widths)
    }

    // --- 8.1.3 applying a layout -------------------------------------------

    @Test
    fun applyingALayoutPinsSelectionAndOrder() {
        val cols = JsonlColumns(defaultCount = 2)
        cols.observe(record(1, """{"timestamp":"t","level":"info","message":"m","file":"f"}"""))
        // Deterministic order would be timestamp, level, message, file.
        cols.applyLayout(JsonlColumnLayout(listOf("file", "message", "level"), emptyMap()))

        assertTrue(cols.isPinned())
        assertEquals(listOf("file", "message", "level"), cols.visible())
    }

    @Test
    fun aLayoutSurvivesLaterDiscoveries() {
        val cols = JsonlColumns(defaultCount = 2)
        cols.observe(record(1, """{"level":"info","message":"m"}"""))
        cols.applyLayout(JsonlColumnLayout(listOf("message", "level"), emptyMap()))
        cols.observe(record(2, """{"level":"warn","latecomer":1}"""))

        assertTrue("latecomer" in cols.available())
        assertEquals(listOf("message", "level"), cols.visible())
    }

    // --- 8.1.4 a column this run never emits --------------------------------

    @Test
    fun aRememberedColumnMissingFromThisRunIsKept() {
        // §3.1.9.4: this run may be the anomaly. Dropping it silently would
        // make the setting feel unreliable.
        val cols = JsonlColumns(defaultCount = 2)
        cols.observe(record(1, """{"level":"info"}"""))
        cols.applyLayout(JsonlColumnLayout(listOf("level", "file"), emptyMap()))

        assertEquals(listOf("level", "file"), cols.visible())
        assertEquals(listOf("level", "file"), cols.currentLayout().columns)
    }

    // --- 8.1.5 width memory -------------------------------------------------

    @Test
    fun onlyUserSetWidthsAreRemembered() {
        val cols = JsonlColumns(defaultCount = 3)
        cols.observe(record(1, """{"level":"info","message":"a long message here"}"""))
        cols.setUserWidth("message", 640)

        val layout = cols.currentLayout()
        assertEquals(mapOf("message" to 640), layout.widths)
        assertEquals(640, cols.userWidth("message"))
        assertNull(cols.userWidth("level"))
    }

    @Test
    fun anAppliedLayoutRestoresItsUserWidths() {
        val cols = JsonlColumns(defaultCount = 3)
        cols.observe(record(1, """{"level":"info","message":"m"}"""))
        cols.applyLayout(JsonlColumnLayout(listOf("level", "message"), mapOf("message" to 500)))

        assertEquals(500, cols.userWidth("message"))
        assertNull(cols.userWidth("level"))
        // ...and re-capturing keeps it, so a session that changes nothing
        // saves back what it loaded.
        assertEquals(mapOf("message" to 500), cols.currentLayout().widths)
    }

    @Test
    fun currentLayoutReflectsTheLiveSelection() {
        val cols = JsonlColumns(defaultCount = 2)
        cols.observe(record(1, """{"level":"info","message":"m","file":"f"}"""))
        cols.setFieldVisible("file", true)
        cols.setFieldVisible("level", false)

        assertEquals(listOf("message", "file"), cols.currentLayout().columns)
    }

    @Test
    fun anUnpinnedColumnsStillReportsItsDefaultAsALayout() {
        // Saving before the reader touches anything must not write nonsense:
        // what it captures is exactly what is on screen.
        val cols = JsonlColumns(defaultCount = 2)
        cols.observe(record(1, """{"level":"info","message":"m","file":"f"}"""))

        assertFalse(cols.isPinned())
        assertEquals(listOf("level", "message"), cols.currentLayout().columns)
    }

    // --- the whole cycle a profile goes through, end to end ----------------

    @Test
    fun aSessionsArrangementIsWhatTheNextSessionOpensWith() {
        // Stand in for the persistent store: encode/decode is the real thing,
        // only the storage is a map. This is the contract the console wiring
        // has to honour — it caught nothing about Swing listeners, so the
        // wiring is verified live (plan 8.3.2), but the model half is pinned.
        val stored = HashMap<String, String>()
        val key = "debug/demo.Prog.main"

        // Session 1: reader trims the columns, reorders, widens one.
        val first = JsonlColumns(defaultCount = 5)
        first.observe(record(1, """{"timestamp":"t","level":"info","message":"m","file":"f","line":9}"""))
        first.setFieldVisible("timestamp", false)
        first.setFieldVisible("line", false)
        first.setOrder(listOf("message", "level", "file"))
        first.setUserWidth("message", 700)
        stored[key] = first.currentLayout().encode()

        // Session 2: a fresh console for the same profile.
        val second = JsonlColumns(defaultCount = 5)
        JsonlColumnLayout.decode(stored[key])?.let { second.applyLayout(it) }

        assertEquals(listOf("message", "level", "file"), second.visible())
        assertEquals(700, second.userWidth("message"))

        // ...and it survives the records of session 2 arriving, including a
        // field the reader had switched off.
        second.observe(record(1, """{"timestamp":"t","level":"warn","message":"m","file":"f","line":3}"""))
        assertEquals(listOf("message", "level", "file"), second.visible())
    }

    @Test
    fun profilesDoNotShareLayouts() {
        val stored = HashMap<String, String>()
        stored["debug/demo.A.main"] = JsonlColumnLayout(listOf("level"), emptyMap()).encode()

        assertNull(JsonlColumnLayout.decode(stored["debug/demo.B.main"]))
        assertEquals(listOf("level"), JsonlColumnLayout.decode(stored["debug/demo.A.main"])?.columns)
    }
}
