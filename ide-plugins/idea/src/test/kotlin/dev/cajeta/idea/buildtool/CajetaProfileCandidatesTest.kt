package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Profile discovery, pure half. The point of these is the failure modes: a
 * dropdown is only safe if "found nothing" and "could not look" stay
 * distinguishable, and if neither one can block a developer from typing a
 * value the scan missed.
 */
class CajetaProfileCandidatesTest {

    @Test
    fun theCompilersAnswerBecomesTheOfferedList() {
        val r = CajetaProfileCandidates.parse("""{"profiles":["dev","prod","test"]}""")
        assertTrue(r.queried)
        assertEquals(listOf("dev", "prod", "test"), r.profiles)
        assertNull(r.emptyMessage())
        // The AOT default leads, and is not repeated when also declared.
        assertEquals(listOf("prod", "dev", "test"), r.offered())
    }

    @Test
    fun aProjectWithNoProfilesSaysSo() {
        val r = CajetaProfileCandidates.parse("""{"profiles":[]}""")
        assertTrue(r.queried)
        assertTrue(r.profiles.isEmpty())
        assertEquals("This project declares no @Profile annotations.", r.emptyMessage())
        // ...and the default is still offered, so the field is never empty.
        assertEquals(listOf("prod"), r.offered())
    }

    @Test
    fun aFailedQueryIsNotAnEmptyProject() {
        // The distinction the whole design rests on.
        for (bad in listOf("", "   ", "cajeta: error: no such directory",
                           "{not json", """{"other":[]}""")) {
            val r = CajetaProfileCandidates.parse(bad)
            assertFalse("'$bad' must not read as a queried project", r.queried)
            assertTrue(r.profiles.isEmpty())
            assertTrue(r.emptyMessage()!!.contains("type one") ||
                       r.emptyMessage()!!.isNotBlank())
        }
    }

    @Test
    fun aBannerBeforeTheJsonIsTolerated() {
        val r = CajetaProfileCandidates.parse(
            "resolving dependencies...\n{\"profiles\":[\"staging\"]}\n")
        assertTrue(r.queried)
        assertEquals(listOf("staging"), r.profiles)
    }

    @Test
    fun blanksAndDuplicatesAreCleanedUp() {
        val r = CajetaProfileCandidates.parse("""{"profiles":["b","","a","b"]}""")
        assertEquals(listOf("a", "b"), r.profiles)
    }

    @Test
    fun theQueryArgvIsSpelledOnce() {
        assertEquals(
            listOf("/usr/bin/cajeta", "--lint", "/proj/src", "--list-profiles"),
            CajetaProfileCandidates.argvFor("/usr/bin/cajeta", "/proj/src"))
    }
}
