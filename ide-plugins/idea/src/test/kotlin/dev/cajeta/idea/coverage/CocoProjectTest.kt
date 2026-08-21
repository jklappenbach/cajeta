package dev.cajeta.idea.coverage

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ide-coverage-plan Unit 4.1 — reading a project's manifest to answer three
 * questions Run with Coverage cannot proceed without: is coco available, which
 * task measures coverage, and where do the artifacts land.
 *
 * Pure, because these are the decisions worth pinning and none of them needs an
 * IDE. Parsing is TOTAL by the [dev.cajeta.idea.buildtool.CajetaManifest]
 * precedent: a half-written `cajeta.json` is the normal state while someone is
 * editing it, so malformed input reports "not configured", never an exception.
 */
class CocoProjectTest {

    /**
     * The shape a real manifest has: a TOP-LEVEL `plugins` block whose entries
     * are objects carrying `version` and `config`, under the id the resolver
     * actually publishes (`dev.cajeta.coverage`). Taken from
     * `samples/tour/coco/cajeta.json`, which is a manifest the build tool runs.
     */
    private val withCoverage = """
        {
            // A comment, because the manifest is JSONC.
            "plugins": {
                "dev.cajeta.coverage": {
                    "version": "0.3.*",
                    "config": { "min": 80, "out": "build/coco" }
                }
            },
            "tasks": {
                "build": { "actions": [ { "action": "build" } ] },
                "test": {
                    "actions": [
                        { "action": "cajeta.coverage.instrument", "id": "ci" },
                        { "action": "test", "instrumented-by": "${'$'}{ci.path}", "id": "tr" },
                        { "action": "cajeta.coverage.report", "min": 80, "id": "cov" }
                    ]
                }
            }
        }
    """.trimIndent()

    // --- 4.1.c  a project without coco explains what is missing --------------

    @Test
    fun aManifestWithNoCoveragePluginIsReportedAsNotConfigured() {
        val s = CocoProject.parse("""{ "tasks": { "build": { "actions": [ { "action": "build" } ] } } }""")
        assertFalse(s.isConfigured)
        assertNotNull("the reason is stated, not left blank", s.problem)
        assertTrue(
            "names the plugin the project would have to declare: ${s.problem}",
            s.problem!!.contains("cajeta.coverage"),
        )
    }

    @Test
    fun aPluginDeclaredButNoTaskUsingItIsADistinctProblem() {
        // Declaring the plugin and never binding it to a task is a different
        // mistake from not having it at all, and the fix is different too.
        val text = """
            {
                "settings": { "plugins": { "cajeta.coverage": "1.0.*" } },
                "tasks": { "build": { "actions": [ { "action": "build" } ] } }
            }
        """.trimIndent()
        val s = CocoProject.parse(text)
        assertFalse(s.isConfigured)
        assertTrue("the plugin was seen", s.pluginDeclared)
        assertTrue(
            "says no task invokes it: ${s.problem}",
            s.problem!!.contains("task"),
        )
    }

    @Test
    fun malformedManifestReportsNotConfiguredRatherThanThrowing() {
        // Half-written JSON is the normal state while editing. It must not
        // surface as a stack trace from the Run action.
        for (bad in listOf("", "{", "{ \"tasks\": ", "not json at all")) {
            val s = CocoProject.parse(bad)
            assertFalse("`$bad` is not configured", s.isConfigured)
            assertNotNull("`$bad` still explains itself", s.problem)
        }
    }

    // --- which task measures coverage ----------------------------------------

    @Test
    fun theTaskCarryingACoverageActionIsTheCoverageTask() {
        val s = CocoProject.parse(withCoverage)
        assertTrue(s.isConfigured)
        assertNull("nothing missing", s.problem)
        assertEquals(listOf("test"), s.coverageTasks)
        assertEquals("test", s.defaultTask)
    }

    @Test
    fun aTaskIsCoverageOnlyWhenItInstrumentsNotMerelyReports() {
        // A report-only task renders what a previous run measured. Running it
        // as "Run with Coverage" would produce a report over stale probe data
        // and look like it had measured something.
        val text = """
            {
                "settings": { "plugins": { "cajeta.coverage": "1.0.*" } },
                "tasks": {
                    "report": { "actions": [ { "action": "cajeta.coverage.report" } ] },
                    "verify": { "actions": [ { "action": "cajeta.coverage.instrument" } ] }
                }
            }
        """.trimIndent()
        val s = CocoProject.parse(text)
        assertEquals(listOf("verify"), s.coverageTasks)
    }

    @Test
    fun severalCoverageTasksAreAllOfferedInManifestOrder() {
        val text = """
            {
                "settings": { "plugins": { "cajeta.coverage": "1.0.*" } },
                "tasks": {
                    "test":     { "actions": [ { "action": "cajeta.coverage.instrument" } ] },
                    "test-slow":{ "actions": [ { "action": "cajeta.coverage.instrument" } ] }
                }
            }
        """.trimIndent()
        val s = CocoProject.parse(text)
        assertEquals(listOf("test", "test-slow"), s.coverageTasks)
        assertEquals("the first is the default, not an arbitrary one", "test", s.defaultTask)
    }

    // --- where the artifacts land --------------------------------------------

    @Test
    fun theOutDirectoryComesFromPluginConfigWhenSet() {
        assertEquals("build/coco", CocoProject.parse(withCoverage).outDir)
    }

    @Test
    fun anActionLevelOutOverridesThePluginConfig() {
        // Per-action params override the plugin's config block (BuildTool.md
        // "config ... applies unless the task overrides it").
        val text = """
            {
                "settings": {
                    "plugins": { "cajeta.coverage": "1.0.*" },
                    "plugin-config": { "cajeta.coverage": { "out": "build/coco" } }
                },
                "tasks": {
                    "test": {
                        "actions": [
                            { "action": "cajeta.coverage.instrument", "out": "target/cov" }
                        ]
                    }
                }
            }
        """.trimIndent()
        assertEquals("target/cov", CocoProject.parse(text).outDir)
    }

    @Test
    fun theOutDirectoryDefaultsToBuildCocoWhenUnset() {
        val text = """
            {
                "settings": { "plugins": { "cajeta.coverage": "1.0.*" } },
                "tasks": { "test": { "actions": [ { "action": "cajeta.coverage.instrument" } ] } }
            }
        """.trimIndent()
        assertEquals(CocoProject.DEFAULT_OUT_DIR, CocoProject.parse(text).outDir)
    }

    @Test
    fun aTopLevelPluginsBlockIsAcceptedAsWellAsOneUnderSettings() {
        // Both spellings appear in the wild; refusing one would report a
        // correctly-configured project as missing coco.
        val text = """
            {
                "plugins": { "cajeta.coverage": "1.0.*" },
                "tasks": { "test": { "actions": [ { "action": "cajeta.coverage.instrument" } ] } }
            }
        """.trimIndent()
        assertTrue(CocoProject.parse(text).isConfigured)
    }

    // --- the id and config shape the build tool actually implements ---------

    @Test
    fun theShippingPluginIdIsRecognised() {
        // `dev.cajeta.coverage` is what the resolver publishes and what
        // samples/tour/coco declares. Matching only the first-party
        // `cajeta.coverage` spelling reported that project as not using coco.
        val text = """
            {
                "plugins": { "dev.cajeta.coverage": { "version": "0.3.*" } },
                "tasks": { "build": { "actions": [ { "action": "build" } ] } }
            }
        """.trimIndent()
        val s = CocoProject.parse(text)
        assertTrue("the shipping id counts as declared", s.pluginDeclared)
    }

    @Test
    fun theOutDirectoryIsReadFromPluginsIdConfig() {
        // `plugins.<id>.config` is the ONLY config shape the build tool
        // implements (Plugin.cpp reads `config`; the key `plugin-config`
        // appears nowhere in src/cajeta/buildtool). Reading only the latter
        // silently reported every custom `out` as the default.
        val text = """
            {
                "plugins": {
                    "dev.cajeta.coverage": {
                        "version": "0.3.*",
                        "config": { "out": "target/coverage" }
                    }
                },
                "tasks": { "test": { "actions": [ { "action": "cajeta.coverage.instrument" } ] } }
            }
        """.trimIndent()
        assertEquals("target/coverage", CocoProject.parse(text).outDir)
    }

    @Test
    fun anActionLevelOutStillBeatsPluginsIdConfig() {
        val text = """
            {
                "plugins": {
                    "dev.cajeta.coverage": {
                        "version": "0.3.*",
                        "config": { "out": "target/coverage" }
                    }
                },
                "tasks": {
                    "test": {
                        "actions": [
                            { "action": "cajeta.coverage.instrument", "out": "target/cov" }
                        ]
                    }
                }
            }
        """.trimIndent()
        assertEquals("target/cov", CocoProject.parse(text).outDir)
    }

    @Test
    fun aStringValuedPluginEntryDeclaresWithoutConfig() {
        // `"plugins": { "id": "1.0.*" }` is the shorthand form. It carries no
        // config, and asking it for one must not throw.
        val text = """
            {
                "plugins": { "dev.cajeta.coverage": "0.3.*" },
                "tasks": { "test": { "actions": [ { "action": "cajeta.coverage.instrument" } ] } }
            }
        """.trimIndent()
        val s = CocoProject.parse(text)
        assertTrue(s.pluginDeclared)
        assertEquals(CocoProject.DEFAULT_OUT_DIR, s.outDir)
    }
}
