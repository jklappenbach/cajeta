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
        // What the project declares, in order — no synthetic entry ahead of
        // them, and the first is what an unconfigured field selects.
        assertEquals(listOf("dev", "prod", "test"), r.offered())
        assertEquals("dev", r.defaultSelection())
    }

    @Test
    fun aProjectWithNoProfilesSaysSo() {
        val r = CajetaProfileCandidates.parse("""{"profiles":[]}""")
        assertTrue(r.queried)
        assertTrue(r.profiles.isEmpty())
        assertEquals("This project declares no @Profile annotations.", r.emptyMessage())
        // ...and the AOT default is the sole fallback, so the field is never
        // empty and never offers a profile that selects nothing.
        assertEquals(listOf("prod"), r.offered())
        assertEquals("prod", r.defaultSelection())
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
    fun aFailedQueryFallsBackToTheAotDefault() {
        val r = CajetaProfileCandidates.parse("boom")
        assertEquals("prod", r.defaultSelection())
        assertEquals(listOf("prod"), r.offered())
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

/**
 * The per-configuration memory's own rules, exercised through a plain map so
 * the precedence is pinned without a live project.
 */
class CajetaProfileMemoryRulesTest {

    /** The precedence the selector applies: remembered, then what is already
     *  in the box, then the first discovered profile. */
    private fun resolve(remembered: String?, typed: String, discovered: List<String>): String {
        val result = CajetaProfileCandidates.Result(discovered, queried = true)
        return remembered?.ifBlank { null }
            ?: typed.ifBlank { null }
            ?: result.defaultSelection()
    }

    @Test
    fun aRememberedChoiceWins() {
        assertEquals("test", resolve("test", "dev", listOf("dev", "prod", "test")))
    }

    @Test
    fun withoutMemoryTheFirstDiscoveredProfileIsUsed() {
        assertEquals("dev", resolve(null, "", listOf("dev", "prod", "test")))
    }

    @Test
    fun discoveryNeverOverridesWhatIsAlreadyInTheBox() {
        // The developer typed something the scan does not know; it stays.
        assertEquals("staging", resolve(null, "staging", listOf("dev", "prod")))
    }

    @Test
    fun withNothingAnywhereTheAotDefaultApplies() {
        assertEquals("prod", resolve(null, "", emptyList()))
        assertEquals("prod", resolve("", "", emptyList()))
    }
}
