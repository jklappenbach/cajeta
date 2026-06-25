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
