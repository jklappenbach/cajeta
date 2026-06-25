package dev.cajeta.idea.jsonl

import com.intellij.openapi.fileTypes.FileType
import javax.swing.Icon

/**
 * The `.jsonl` / `.ndjson` / `.jsonlines` file type (spec §8.1). Text-based (not
 * binary) so the platform's text editor opens it as usual; the structured view is
 * added alongside by [JsonlFileEditorProvider]. Registered in plugin.xml with
 * `fieldName="INSTANCE"` (the Kotlin object singleton).
 */
object JsonlFileType : FileType {
    override fun getName(): String = "JSONL"
    override fun getDescription(): String = "Newline-delimited JSON (JSONL / NDJSON)"
    override fun getDefaultExtension(): String = "jsonl"
    override fun getIcon(): Icon? = null
    override fun isBinary(): Boolean = false
    override fun isReadOnly(): Boolean = false
}
