package dev.cajeta.idea

import com.intellij.openapi.fileTypes.LanguageFileType
import javax.swing.Icon

object CajetaFileType : LanguageFileType(CajetaLanguage) {
    override fun getName(): String = "Cajeta"
    override fun getDescription(): String = "Cajeta source file"
    override fun getDefaultExtension(): String = "cajeta"
    override fun getIcon(): Icon = CajetaIcons.FILE
}
