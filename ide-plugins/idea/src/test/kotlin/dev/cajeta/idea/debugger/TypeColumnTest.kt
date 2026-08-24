package dev.cajeta.idea.debugger

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * variable-inspection §4.1.5: the type column shows the SIMPLE declared type
 * (`String[]`, `Point`), not the fully-qualified name the server sends. The
 * server deliberately reports the canonical FQN — `ValueInspector::runtimeType`
 * returns `resolveObject().type`, which is the vtable's canonical name — so the
 * shortening is a presentation decision and belongs here, not on the wire.
 *
 * Pure string work, so it needs no platform: the shortener is what
 * `CajetaValue.computePresentation` feeds to the type column.
 */
class TypeColumnTest {

    @Test
    fun qualifiedNameShortensToItsLastSegment() {
        assertEquals("String", TypeColumn.short("cajeta.lang.String"))
        assertEquals("Point", TypeColumn.short("tour.Point"))
    }

    @Test
    fun arraySuffixSurvivesShortening() {
        assertEquals("String[]", TypeColumn.short("cajeta.lang.String[]"))
        assertEquals("Point[][]", TypeColumn.short("tour.geom.Point[][]"))
    }

    @Test
    fun genericArgumentsShortenToo() {
        assertEquals(
            "ArrayList<String>",
            TypeColumn.short("cajeta.collection.ArrayList<cajeta.lang.String>"),
        )
        assertEquals(
            "HashMap<String, Point>",
            TypeColumn.short("cajeta.collection.HashMap<cajeta.lang.String, tour.Point>"),
        )
        assertEquals(
            "ArrayList<ArrayList<int32>>",
            TypeColumn.short("cajeta.collection.ArrayList<cajeta.collection.ArrayList<int32>>"),
        )
    }

    @Test
    fun primitivesAndAlreadyShortNamesAreUntouched() {
        assertEquals("int32", TypeColumn.short("int32"))
        assertEquals("bool", TypeColumn.short("bool"))
        assertEquals("Point", TypeColumn.short("Point"))
        assertEquals("int32[]", TypeColumn.short("int32[]"))
    }

    @Test
    fun blankStaysBlankAndNeverBecomesADot() {
        // A blank type means "the server told us nothing"; the caller turns it
        // into a null type column. Shortening must not invent a segment.
        assertEquals("", TypeColumn.short(""))
        assertEquals("", TypeColumn.short("   "))
    }

    @Test
    fun aTrailingDotDoesNotYieldAnEmptyName() {
        // Defensive: a malformed name must degrade to something readable
        // rather than vanishing from the column entirely.
        assertEquals("cajeta.lang.", TypeColumn.short("cajeta.lang."))
    }
}
