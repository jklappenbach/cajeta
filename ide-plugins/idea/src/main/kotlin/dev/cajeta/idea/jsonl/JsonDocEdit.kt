package dev.cajeta.idea.jsonl

import dev.cajeta.idea.debugger.Json

/**
 * Structured edits over the span tree (json-viewer spec §4.1.3): every
 * operation is ONE contiguous [TextEdit], so untouched regions stay
 * byte-identical and one edit maps to one undoable Document action. The caller
 * reparses after applying — spans are only valid against the text they were
 * parsed from.
 */
object JsonDocEdit {

    /** Replace [start, end) with [replacement]; start == end is a pure insertion. */
    data class TextEdit(val start: Int, val end: Int, val replacement: String)

    fun apply(source: String, edit: TextEdit): String =
        source.substring(0, edit.start) + edit.replacement + source.substring(edit.end)

    /** Replace a scalar's token with the new value's compact serialization. */
    fun setScalar(node: JsonDocNode.Scalar, newValue: Json): TextEdit =
        TextEdit(node.span.start, node.span.end, newValue.toCompactString())

    /** Append a key after the object's last entry (inside the braces when empty). */
    fun addKey(obj: JsonDocNode.ObjectNode, key: String, value: Json): TextEdit {
        val serialized = Json.obj(key to value).toCompactString().let {
            it.substring(1, it.length - 1)   // `"key":value` with proper key escaping
        }
        val last = obj.entries.lastOrNull()
            ?: return TextEdit(obj.span.start + 1, obj.span.start + 1, serialized)
        val at = last.value.span.end
        return TextEdit(at, at, ", $serialized")
    }

    /** Cut one element and exactly one adjacent comma (the whole array's inner
     *  content when it is the sole element). */
    fun removeElement(arr: JsonDocNode.ArrayNode, index: Int): TextEdit {
        val elem = arr.elements[index]
        return when {
            arr.elements.size == 1 -> TextEdit(arr.span.start + 1, arr.span.end - 1, "")
            index == 0 -> TextEdit(elem.span.start, arr.elements[1].span.start, "")
            else -> TextEdit(arr.elements[index - 1].span.end, elem.span.end, "")
        }
    }

    /** Cut one entry and exactly one adjacent comma (the whole object's inner
     *  content when it is the sole entry). */
    fun removeKey(obj: JsonDocNode.ObjectNode, key: String): TextEdit {
        val index = obj.entries.indexOfFirst { it.key == key }
        require(index >= 0) { "no key '$key'" }
        val entry = obj.entries[index]
        return when {
            obj.entries.size == 1 -> TextEdit(obj.span.start + 1, obj.span.end - 1, "")
            index == 0 -> TextEdit(entry.keySpan.start, obj.entries[1].keySpan.start, "")
            else -> TextEdit(obj.entries[index - 1].value.span.end, entry.value.span.end, "")
        }
    }

    /** Documents above this size open read-only with a banner (spec §4.1.5). */
    const val EDITABLE_MAX_BYTES = 4L * 1024 * 1024

    fun isEditableSize(lengthBytes: Long): Boolean = lengthBytes <= EDITABLE_MAX_BYTES
}
