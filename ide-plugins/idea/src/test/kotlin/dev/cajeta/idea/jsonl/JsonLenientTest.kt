package dev.cajeta.idea.jsonl

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * json-viewer unit 1 (spec §2.1.4, 1.3.2 drafted JSONC-only): `//` and `/* */`
 * comments and trailing commas parse leniently — in documents and JSONL lines —
 * with spans still correct; strict mode rejects them (the engine's strict-first
 * classification depends on that).
 */
class JsonLenientTest {

    private val jsonc = """
        {
          // toolchain pin
          "llvm": "cajeta-llvm", /* RTTI fork */
          "targets": [
            "debug",
            "release",
          ],
        }
    """.trimIndent()

    @Test
    fun commentsAndTrailingCommasParseLeniently() {
        val root = (JsonDocModel.parse(jsonc) as JsonDocResult.Ok).root as JsonDocNode.ObjectNode
        assertEquals(listOf("llvm", "targets"), root.entries.map { it.key })
        val targets = root.entries[1].value as JsonDocNode.ArrayNode
        assertEquals(2, targets.elements.size)
    }

    @Test
    fun spansStayCorrectUnderLeniency() {
        val root = (JsonDocModel.parse(jsonc) as JsonDocResult.Ok).root as JsonDocNode.ObjectNode
        val llvm = root.entries[0]
        assertEquals("\"llvm\"", jsonc.substring(llvm.keySpan.start, llvm.keySpan.end))
        assertEquals("\"cajeta-llvm\"", jsonc.substring(llvm.value.span.start, llvm.value.span.end))
    }

    @Test
    fun strictModeRejectsCommentsAndTrailingCommas() {
        assertTrue(JsonDocModel.parse(jsonc, lenient = false) is JsonDocResult.Err)
        assertTrue(JsonDocModel.parse("""{"a": 1,}""", lenient = false) is JsonDocResult.Err)
    }

    @Test
    fun lenientJsonlLineBecomesARecord() {
        val row = JsonlEngine.parseLine(1, """{"level":"info","message":"m",}""")
        assertTrue(row is JsonlRow.Record)
        assertEquals("info", (row as JsonlRow.Record).level)
    }
}
