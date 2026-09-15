package dev.cajeta.idea.xref

import com.intellij.openapi.editor.Document
import com.intellij.openapi.editor.EditorFactory
import com.intellij.openapi.editor.RangeMarker
import com.intellij.openapi.editor.event.EditorFactoryEvent
import com.intellij.openapi.editor.event.EditorFactoryListener
import com.intellij.openapi.util.Key
import dev.cajeta.idea.debugger.Json
import dev.cajeta.idea.lint.XrefRecord

/**
 * Pins a lint's use positions to the open document they were computed from, as
 * range markers that move with edits (parse-error-locality §4). A use whose
 * line moved before the next successful lint still finds its shard record.
 */
object CajetaXrefPositions {

    data class Pos(val line: Int, val col: Int)

    private class Anchors(val markers: List<Pair<RangeMarker, Pos>>) {
        fun dispose() = markers.forEach { it.first.dispose() }
    }

    private val KEY = Key.create<Anchors>("cajeta.xref.positions")

    /** Pin every use record in [records] to [document], whose text must be the
     *  text the records were computed from. Replaces any earlier pins. */
    fun anchor(document: Document, records: List<XrefRecord>) {
        clear(document)
        val markers = ArrayList<Pair<RangeMarker, Pos>>()
        for (r in records) {
            if (r.rel != "references" && r.rel != "calls") continue
            val line = (r.record.opt("line") as? Json.Num)?.value?.toInt() ?: continue
            val col = (r.record.opt("col") as? Json.Num)?.value?.toInt() ?: continue
            if (line < 1 || line > document.lineCount) continue
            val offset = document.getLineStartOffset(line - 1) + col
            if (offset > document.textLength) continue
            markers += document.createRangeMarker(offset, offset) to Pos(line, col)
        }
        document.putUserData(KEY, Anchors(markers))
    }

    /** The recorded position of the use now pinned at [offset], or null. */
    fun recordedPositionAt(document: Document, offset: Int): Pos? =
        document.getUserData(KEY)?.markers
            ?.firstOrNull { (m, _) -> m.isValid && m.startOffset == offset }
            ?.second

    fun clear(document: Document) {
        document.getUserData(KEY)?.dispose()
        document.putUserData(KEY, null)
    }

    /** Drops a document's pins when its last editor closes. */
    class EditorClosed : EditorFactoryListener {
        override fun editorReleased(event: EditorFactoryEvent) {
            val doc = event.editor.document
            val others = EditorFactory.getInstance().getEditors(doc)
                .any { it !== event.editor }
            if (!others) clear(doc)
        }
    }
}
