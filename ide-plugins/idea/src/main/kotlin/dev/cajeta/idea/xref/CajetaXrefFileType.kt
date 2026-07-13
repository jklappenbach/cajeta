package dev.cajeta.idea.xref

import com.intellij.openapi.fileTypes.FileType
import com.intellij.openapi.util.NlsSafe
import dev.cajeta.idea.CajetaIcons
import javax.swing.Icon

/**
 * The `.cjxref` shard file type (ide-symbol-index Unit 6). Text-based so the
 * FileBasedIndex receives content; otherwise inert — no language, no editor
 * features. These files are plugin-written cache under `.cajeta/xref/`.
 */
object CajetaXrefFileType : FileType {
    override fun getName(): @NlsSafe String = "Cajeta Xref Shard"
    override fun getDescription(): String = "Cajeta cross-reference index shard"
    override fun getDefaultExtension(): String = "cjxref"
    override fun getIcon(): Icon = CajetaIcons.FILE
    override fun isBinary(): Boolean = false
    override fun isReadOnly(): Boolean = true
}
