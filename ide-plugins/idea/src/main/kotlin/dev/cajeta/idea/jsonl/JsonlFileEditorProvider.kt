package dev.cajeta.idea.jsonl

import com.intellij.openapi.fileEditor.FileEditor
import com.intellij.openapi.fileEditor.FileEditorPolicy
import com.intellij.openapi.fileEditor.FileEditorProvider
import com.intellij.openapi.project.DumbAware
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile

/**
 * Adds the structured JSONL view as a tab alongside the plain text editor for
 * `.jsonl` / `.ndjson` / `.jsonlines` files (spec §8.1, §8.2.1). Placed after the
 * default text editor so a developer can switch between verbatim text and the
 * structured rendering. The structured editor reuses the shared §8 engine via the
 * windowing reader so large files open responsively (§8.2.2).
 */
class JsonlFileEditorProvider : FileEditorProvider, DumbAware {

    override fun accept(project: Project, file: VirtualFile): Boolean =
        file.extension?.lowercase() in EXTENSIONS

    override fun createEditor(project: Project, file: VirtualFile): FileEditor =
        JsonlStructuredEditor(file)

    override fun getEditorTypeId(): String = "cajeta-jsonl-structured"

    override fun getPolicy(): FileEditorPolicy = FileEditorPolicy.PLACE_AFTER_DEFAULT_EDITOR

    companion object {
        private val EXTENSIONS = setOf("jsonl", "ndjson", "jsonlines")
    }
}
