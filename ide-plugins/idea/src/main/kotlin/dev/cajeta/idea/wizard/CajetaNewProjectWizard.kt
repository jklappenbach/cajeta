package dev.cajeta.idea.wizard

import com.intellij.ide.wizard.AbstractNewProjectWizardStep
import com.intellij.ide.wizard.NewProjectWizardBaseData.Companion.baseData
import com.intellij.ide.wizard.NewProjectWizardStep
import com.intellij.ide.wizard.language.LanguageGeneratorNewProjectWizard
import com.intellij.openapi.application.invokeLater
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.openapi.vfs.VirtualFileManager
import com.intellij.ui.dsl.builder.Panel
import com.intellij.ui.dsl.builder.bindText
import com.intellij.ui.dsl.builder.columns
import dev.cajeta.idea.CajetaIcons
import dev.cajeta.idea.CajetaLanguage
import java.nio.file.Path
import javax.swing.Icon

/**
 * Adds "Cajeta" to the language list in File → New → Project.
 * Uses the post-2023.1 generator API which is stable across the
 * IntelliJ versions we target (242 — 261.*).
 */
class CajetaNewProjectWizard : LanguageGeneratorNewProjectWizard {

    override val name: String = CajetaLanguage.id
    override val ordinal: Int = 600
    override val icon: Icon get() = CajetaIcons.FILE

    override fun createStep(parent: NewProjectWizardStep): NewProjectWizardStep =
        CajetaWizardStep(parent)
}

private class CajetaWizardStep(parent: NewProjectWizardStep) :
    AbstractNewProjectWizardStep(parent) {

    private val packageProp = propertyGraph.lazyProperty { defaultPackageName() }
    private val entryClassProp = propertyGraph.lazyProperty { "App" }

    override fun setupUI(builder: Panel) {
        with(builder) {
            row("Package:") {
                textField()
                    .bindText(packageProp)
                    .columns(40)
                    .comment("Dot-separated identifier; becomes the directory under src/.")
            }
            row("Entry class:") {
                textField()
                    .bindText(entryClassProp)
                    .columns(40)
                    .comment("Class containing the static main() method.")
            }
        }
    }

    override fun setupProject(project: Project) {
        val baseDir = context.projectDirectory
        val params = CajetaProjectScaffold.Params(
            projectRoot = Path.of(baseDir.toString()),
            projectName = baseData?.name ?: "cajeta-app",
            packageName = packageProp.get(),
            entryClass = entryClassProp.get(),
        )
        CajetaProjectScaffold.materialize(params)

        // Refresh the VFS so the scaffolded files become visible
        // in the Project tool window without a manual sync.
        invokeLater {
            VirtualFileManager.getInstance().syncRefresh()
            LocalFileSystem.getInstance().refreshAndFindFileByNioFile(params.projectRoot)
        }
    }

    private fun defaultPackageName(): String {
        val rawName = baseData?.name ?: "app"
        val sanitized = rawName.lowercase()
            .replace(Regex("[^a-z0-9_]+"), "_")
            .trimStart('_')
        return sanitized.ifEmpty { "app" }
    }
}
