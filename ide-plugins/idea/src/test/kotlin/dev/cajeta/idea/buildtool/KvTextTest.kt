package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Test

/** W-buildtool unit 6 core: newline-delimited key=value parsing for the run
 *  config's -P/-p fields. */
class KvTextTest {

    @Test
    fun parsesTrimsAndDropsBlankOrKeyless() {
        val m = KvText.parse(
            """
            arch = x64
              stack-version=1.5.0

            =novalue
            justtext
            """.trimIndent(),
        )
        assertEquals(mapOf("arch" to "x64", "stack-version" to "1.5.0"), m)
    }

    @Test
    fun formatThenParseRoundTrips() {
        val src = linkedMapOf("a" to "1", "b" to "two")
        assertEquals(src, KvText.parse(KvText.format(src)))
    }

    @Test
    fun emptyTextIsEmptyMap() {
        assertEquals(emptyMap<String, String>(), KvText.parse(""))
    }
}
