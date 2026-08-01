package dev.cajeta.idea.jsonl

import dev.cajeta.idea.debugger.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * json-viewer unit 1 (spec §2.1.1, §2.1.2): the document model shared by all
 * surfaces — a JSON value tree with source spans — and located, all-or-nothing
 * parse errors (never a partial tree).
 */
class JsonDocModelTest {

    private val doc = """
        {
          "name": "cajeta",
          "version": 5,
          "tags": ["ide", "json"],
          "nested": {"deep": true, "pi": 3.14}
        }
    """.trimIndent()

    @Test
    fun parsesNestedDocumentIntoTreeWithSpans() {
        val root = (JsonDocModel.parse(doc) as JsonDocResult.Ok).root as JsonDocNode.ObjectNode
        assertEquals(listOf("name", "version", "tags", "nested"), root.entries.map { it.key })
        // spans slice back to the exact source text of keys and values
        val name = root.entries[0]
        assertEquals("\"name\"", doc.substring(name.keySpan.start, name.keySpan.end))
        assertEquals("\"cajeta\"", doc.substring(name.value.span.start, name.value.span.end))
        val tags = root.entries[2].value as JsonDocNode.ArrayNode
        assertEquals(2, tags.elements.size)
        assertEquals("[\"ide\", \"json\"]", doc.substring(tags.span.start, tags.span.end))
        val nested = root.entries[3].value as JsonDocNode.ObjectNode
        assertEquals("""{"deep": true, "pi": 3.14}""", doc.substring(nested.span.start, nested.span.end))
        // the root span covers the whole document
        assertEquals(doc, doc.substring(root.span.start, root.span.end))
    }

    @Test
    fun scalarsObjectsArraysRoundTripToText() {
        val root = (JsonDocModel.parse(doc) as JsonDocResult.Ok).root
        val compact = root.toJson().toCompactString()
        val reparsed = (JsonDocModel.parse(compact) as JsonDocResult.Ok).root
        assertEquals(root.toJson(), reparsed.toJson())
        assertEquals(Json.parse(compact), root.toJson())
    }

    @Test
    fun malformedLocatesErrorAndYieldsNoPartialTree() {
        val bad = "{\n  \"a\": !\n}"
        val err = JsonDocModel.parse(bad) as JsonDocResult.Err
        assertEquals(2, err.line)
        assertEquals(8, err.column)
        assertTrue(err.message.isNotEmpty())
    }

    @Test
    fun emptyInputIsAnErrorNotACrash() {
        assertTrue(JsonDocModel.parse("") is JsonDocResult.Err)
        assertTrue(JsonDocModel.parse("   ") is JsonDocResult.Err)
    }

    @Test
    fun trailingGarbageAfterTheDocumentIsAnError() {
        assertTrue(JsonDocModel.parse("""{"a": 1} extra""") is JsonDocResult.Err)
    }
}
