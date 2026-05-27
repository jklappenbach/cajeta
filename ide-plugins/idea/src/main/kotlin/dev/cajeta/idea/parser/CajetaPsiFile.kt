package dev.cajeta.idea.parser

import com.intellij.extapi.psi.PsiFileBase
import com.intellij.openapi.fileTypes.FileType
import com.intellij.psi.FileViewProvider
import dev.cajeta.idea.CajetaFileType
import dev.cajeta.idea.CajetaLanguage

class CajetaPsiFile(viewProvider: FileViewProvider) :
    PsiFileBase(viewProvider, CajetaLanguage) {

    override fun getFileType(): FileType = CajetaFileType
    override fun toString(): String = "Cajeta File"
}
