package dev.cajeta.idea.jsonl

import com.intellij.openapi.fileEditor.FileEditor
import com.intellij.openapi.fileEditor.FileEditorPolicy
import com.intellij.openapi.fileEditor.FileEditorProvider
import com.intellij.openapi.project.DumbAware
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile

/**
 * Adds the structured document tab beside the platform text editor for `.json`
 * and `.jsonc` files (json-viewer spec §4.1.1/§4.2.2; the platform editor stays
 * the editing surface of record). `.jsonl`-family files keep their own windowed
 * provider ([JsonlFileEditorProvider]).
 */
class JsonDocFileEditorProvider : FileEditorProvider, DumbAware {

    override fun accept(project: Project, file: VirtualFile): Boolean =
        file.extension?.lowercase() in EXTENSIONS

    override fun createEditor(project: Project, file: VirtualFile): FileEditor =
        JsonDocStructuredEditor(project, file)

    override fun getEditorTypeId(): String = "cajeta-json-structured"

    override fun getPolicy(): FileEditorPolicy = FileEditorPolicy.PLACE_AFTER_DEFAULT_EDITOR

    companion object {
        private val EXTENSIONS = setOf("json", "jsonc")
    }
}
