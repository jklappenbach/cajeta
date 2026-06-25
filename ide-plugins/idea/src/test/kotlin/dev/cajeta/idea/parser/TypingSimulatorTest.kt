package dev.cajeta.idea.parser

import dev.cajeta.idea.harness.Fixture
import dev.cajeta.idea.harness.FixtureLoader
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.nio.file.Path

/**
 * W4c: the headless half of the typing-simulator harness. The interactive
 * [dev.cajeta.idea.harness.TypeFixtureAction] / FixtureTyper types fixtures into
 * a real editor and watches annotations; this types the same fixtures (loaded
 * the same way, via [FixtureLoader]) through the bare parser, with no platform,
 * and asserts the incremental-parse invariants in CI. Fixtures live under
 * `src/test/resources/typing-fixtures/{valid,invalid}/`.
 */
class TypingSimulatorTest {

    private fun fixtureRoot(): Path =
        File(javaClass.getResource("/typing-fixtures")!!.toURI()).toPath()

    private fun fixtures(): List<Fixture> =
        FixtureLoader.load(fixtureRoot()).also {
            assertTrue("no fixtures found under typing-fixtures/", it.isNotEmpty())
        }

    /** No prefix of ANY fixture — valid or invalid — may throw. Incremental
     *  parse must degrade gracefully, never crash. */
    @Test
    fun noFixturePrefixEverCrashes() {
        for (f in fixtures()) {
            val crash = TypingSimulator.simulate(f.text()).firstOrNull { it.crashed }
            assertNull(
                "${f.displayName}: parse threw at prefix len ${crash?.prefixLen}: ${crash?.threw}",
                crash,
            )
        }
    }

    @Test
    fun validFixturesParseCleanWhenComplete() {
        val valid = fixtures().filter { it.kind == Fixture.Kind.VALID }
        assertTrue("no valid fixtures", valid.isNotEmpty())
        for (f in valid) {
            assertEquals(
                "${f.displayName}: complete file should parse with no errors",
                0, TypingSimulator.simulate(f.text()).last().syntaxErrors,
            )
        }
    }

    @Test
    fun invalidFixturesReportErrorsWhenComplete() {
        val invalid = fixtures().filter { it.kind == Fixture.Kind.INVALID }
        assertTrue("no invalid fixtures", invalid.isNotEmpty())
        for (f in invalid) {
            assertTrue(
                "${f.displayName}: complete file should report at least one error",
                TypingSimulator.simulate(f.text()).last().syntaxErrors > 0,
            )
        }
    }
}
