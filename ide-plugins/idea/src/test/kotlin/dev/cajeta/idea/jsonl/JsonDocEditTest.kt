package dev.cajeta.idea.jsonl

import dev.cajeta.idea.debugger.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * json-viewer unit 4 (spec §4.1.3, §4.2.2): structured edits regenerate MINIMAL
 * text — every operation is one contiguous TextEdit, untouched regions stay
 * byte-identical — so a VCS diff after one scalar edit touches only that line.
 * The large-document threshold is a pure gate (§4.1.5).
 */
class JsonDocEditTest {

    private val doc = """
        {
          "name": "cajeta",
          "opts": {"warm": true},
          "tags": ["ide", "json", "viewer"]
        }
    """.trimIndent()

    private fun root(text: String) =
        ((JsonDocModel.parse(text) as JsonDocResult.Ok).root) as JsonDocNode.ObjectNode

    @Test
    fun scalarEditReplacesOnlyThatToken() {
        val r = root(doc)
        val name = r.entries[0].value as JsonDocNode.Scalar
        val edit = JsonDocEdit.setScalar(name, Json.of("cajeta-five"))
        val out = JsonDocEdit.apply(doc, edit)
        // exactly the old token replaced; everything else byte-identical
        assertEquals(doc.replace("\"cajeta\"", "\"cajeta-five\""), out)
        assertEquals(doc.substring(0, edit.start), out.substring(0, edit.start))
        assertEquals(doc.substring(edit.end), out.substring(out.length - (doc.length - edit.end)))
        assertEquals("cajeta-five", root(out).entries[0].value.toJson().asString())
    }

    @Test
    fun keyAddIsAPureInsertionAtOnePoint() {
        val edit = JsonDocEdit.addKey(root(doc), "version", Json.of(5))
        assertEquals(edit.start, edit.end)                       // insertion, no bytes removed
        val out = JsonDocEdit.apply(doc, edit)
        val newRoot = root(out)
        assertEquals(listOf("name", "opts", "tags", "version"), newRoot.entries.map { it.key })
        assertEquals(5, newRoot.entries.last().value.toJson().asInt())
        assertEquals(doc.substring(0, edit.start), out.substring(0, edit.start))
    }

    @Test
    fun keyAddIntoEmptyObjectStaysValid() {
        val src = """{ }"""
        val edit = JsonDocEdit.addKey(root(src), "a", Json.of(true))
        val out = JsonDocEdit.apply(src, edit)
        assertTrue(root(out).entries.single().value.toJson().asBool())
    }

    @Test
    fun elementRemoveCutsOneContiguousRegion() {
        val r = root(doc)
        val tags = r.entries[2].value as JsonDocNode.ArrayNode
        // middle element
        var out = JsonDocEdit.apply(doc, JsonDocEdit.removeElement(tags, 1))
        assertEquals(listOf("ide", "viewer"), tagsOf(out))
        // first element
        out = JsonDocEdit.apply(doc, JsonDocEdit.removeElement(tags, 0))
        assertEquals(listOf("json", "viewer"), tagsOf(out))
        // sole element collapses to []
        val soloSrc = """{"xs": [7]}"""
        val solo = root(soloSrc).entries[0].value as JsonDocNode.ArrayNode
        assertEquals("""{"xs": []}""", JsonDocEdit.apply(soloSrc, JsonDocEdit.removeElement(solo, 0)))
    }

    @Test
    fun keyRemoveCutsOneContiguousRegion() {
        val r = root(doc)
        var out = JsonDocEdit.apply(doc, JsonDocEdit.removeKey(r, "opts"))       // middle
        assertEquals(listOf("name", "tags"), root(out).entries.map { it.key })
        out = JsonDocEdit.apply(doc, JsonDocEdit.removeKey(r, "name"))           // first
        assertEquals(listOf("opts", "tags"), root(out).entries.map { it.key })
        val soleSrc = """{"only": 1}"""
        assertEquals("{}", JsonDocEdit.apply(soleSrc, JsonDocEdit.removeKey(root(soleSrc), "only")))
    }

    @Test
    fun largeDocumentThresholdIsAPureGate() {
        assertTrue(JsonDocEdit.isEditableSize(1024))
        assertTrue(JsonDocEdit.isEditableSize(JsonDocEdit.EDITABLE_MAX_BYTES))
        assertFalse(JsonDocEdit.isEditableSize(JsonDocEdit.EDITABLE_MAX_BYTES + 1))
    }

    private fun tagsOf(text: String): List<String> =
        (root(text).entries[2].value as JsonDocNode.ArrayNode)
            .elements.map { it.toJson().asString() }
}
