package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * W-buildtool unit 3: the plain-JVM task-discovery parser (spec §3.1.4). Drives
 * the `cajeta tasks --json` contract document into the model — full fidelity on
 * the §3 fields, tolerant of unknown fields, and a typed failure (never a throw)
 * on malformed input.
 */
class TaskDiscoveryParserTest {

    private fun success(json: String): TaskModel {
        val r = TaskDiscoveryParser.parse(json)
        assertTrue("expected success, got $r", r is TaskDiscoveryParser.Result.Success)
        return (r as TaskDiscoveryParser.Result.Success).model
    }

    // 3.3: every §3 contract field is represented.
    @Test
    fun parsesFullContractWithAllFields() {
        val json = """
            {
              "manifest": "/abs/path/cajeta.json",
              "tasks": [
                { "name": "build", "description": "Compile + link the project",
                  "dependsOn": ["check"],
                  "params": [ { "name": "flavor", "type": "string", "default": "debug",
                                "required": false, "doc": "Build flavor" } ] }
              ],
              "builtins": [
                { "name": "tasks", "description": "List task names" },
                { "name": "info",  "description": "Print dependency tree / capabilities" }
              ]
            }
        """.trimIndent()

        val m = success(json)
        assertEquals("/abs/path/cajeta.json", m.manifest)
        assertEquals(1, m.tasks.size)
        val build = m.tasks[0]
        assertEquals("build", build.name)
        assertEquals("Compile + link the project", build.description)
        assertEquals(listOf("check"), build.dependsOn)
        assertEquals(1, build.params.size)
        val p = build.params[0]
        assertEquals("flavor", p.name)
        assertEquals("string", p.type)
        assertEquals("debug", p.default)
        assertEquals(false, p.required)
        assertEquals("Build flavor", p.doc)
        assertEquals(listOf("tasks", "info"), m.builtins.map { it.name })
    }

    // §5.2.2: runnable + artifact carry through; default false/null when absent.
    @Test
    fun parsesRunnableAndArtifact() {
        val m = success(
            """
            { "manifest": "/m.json", "tasks": [
              { "name": "build", "runnable": true, "artifact": "build/exe/demo" },
              { "name": "lint" }
            ] }
            """.trimIndent(),
        )
        val build = m.tasks.first { it.name == "build" }
        assertTrue(build.runnable)
        assertEquals("build/exe/demo", build.artifact)
        val lint = m.tasks.first { it.name == "lint" }
        assertTrue(!lint.runnable)
        assertNull(lint.artifact)
    }

    // §5.2.2: the document-level `build` object carries debug-launch coords; a
    // `build` object without an entry method is not debuggable (null coords).
    @Test
    fun parsesDebugLaunchCoordsFromBuildObject() {
        val m = success(
            """
            { "manifest": "/m.json",
              "build": { "sourceRoot": "src/main/cajeta", "entryMethod": "p.P::main" },
              "tasks": [ { "name": "run", "runnable": true } ] }
            """.trimIndent(),
        )
        assertEquals("p.P::main", m.buildCoords!!.entryMethod)
        assertEquals("src/main/cajeta", m.buildCoords!!.sourceRoot)
    }

    @Test
    fun buildObjectWithoutEntryMethodYieldsNullCoords() {
        val m = success("""{ "manifest": "/m.json", "build": { "sourceRoot": "s" }, "tasks": [] }""")
        assertNull(m.buildCoords)
        // and absent entirely -> null
        assertNull(success("""{ "manifest": "/m.json", "tasks": [] }""").buildCoords)
    }

    // §3.1.3 / §3.2.3: an older plugin must ignore fields a newer build tool adds.
    @Test
    fun ignoresUnknownFieldsForwardCompat() {
        val json = """
            {
              "manifest": "/m.json",
              "schemaVersion": 9,
              "tasks": [ { "name": "t", "futureField": {"x": 1}, "params": [
                { "name": "p", "newCap": true } ] } ],
              "builtins": [],
              "extra": [1, 2, 3]
            }
        """.trimIndent()

        val m = success(json)
        assertEquals("/m.json", m.manifest)
        assertEquals("t", m.tasks.single().name)
        assertEquals("p", m.tasks.single().params.single().name)
    }

    // §3.1.3: optional fields default, arrays may be absent.
    @Test
    fun appliesDefaultsForSparseTask() {
        val m = success("""{ "manifest": "/m.json", "tasks": [ { "name": "clean" } ] }""")
        val clean = m.tasks.single()
        assertNull(clean.description)
        assertTrue(clean.dependsOn.isEmpty())
        assertTrue(clean.params.isEmpty())
        assertTrue(m.builtins.isEmpty())
    }

    @Test
    fun malformedJsonIsTypedFailureNotThrow() {
        val r = TaskDiscoveryParser.parse("{ not valid json ")
        assertTrue(r is TaskDiscoveryParser.Result.Failure)
    }

    @Test
    fun nonObjectTopLevelIsFailure() {
        assertTrue(TaskDiscoveryParser.parse("[1,2,3]") is TaskDiscoveryParser.Result.Failure)
    }

    // A nameless task entry is unusable and is dropped, not fatal.
    @Test
    fun skipsNamelessTaskEntries() {
        val m = success("""{ "manifest": "/m.json", "tasks": [ {"description":"x"}, {"name":"ok"} ] }""")
        assertEquals(listOf("ok"), m.tasks.map { it.name })
    }
}
