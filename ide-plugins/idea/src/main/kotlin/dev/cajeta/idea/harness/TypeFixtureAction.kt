package dev.cajeta.idea.harness

import com.intellij.openapi.actionSystem.AnAction
import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.actionSystem.CommonDataKeys
import com.intellij.openapi.fileEditor.FileEditorManager
import com.intellij.openapi.fileEditor.OpenFileDescriptor
import com.intellij.openapi.project.Project
import com.intellij.openapi.ui.Messages
import com.intellij.openapi.ui.popup.JBPopupFactory
import com.intellij.openapi.ui.popup.ListPopup
import com.intellij.openapi.ui.popup.PopupStep
import com.intellij.openapi.ui.popup.util.BaseListPopupStep
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.testFramework.LightVirtualFile
import dev.cajeta.idea.CajetaFileType
import dev.cajeta.idea.settings.CajetaSettings
import java.nio.file.Path
import java.nio.file.Paths

/**
 * Menu action: pick a fixture from the test-code directory and have
 * the typing simulator type it into a fresh scratch editor.
 *
 * Triggered from Tools → Cajeta → Type Fixture into Editor.
 */
class TypeFixtureAction : AnAction("Type Fixture into Editor") {

    override fun actionPerformed(event: AnActionEvent) {
        val project = event.project ?: return
        val settings = CajetaSettings.instance
        val fixturesRoot = Paths.get(settings.testFixturesPath)
        val fixtures = FixtureLoader.load(fixturesRoot)
        if (fixtures.isEmpty()) {
            Messages.showWarningDialog(
                project,
                "No fixtures found under:\n$fixturesRoot\n\nConfigure the path in " +
                    "Settings | Languages & Frameworks | Cajeta.",
                "Cajeta — Type Fixture",
            )
            return
        }
        showPicker(project, fixtures, settings.testTypingDelayMs).showInBestPositionFor(event.dataContext)
    }

    private fun showPicker(project: Project, fixtures: List<Fixture>, delayMs: Int): ListPopup {
        val step = object : BaseListPopupStep<Fixture>("Cajeta Fixtures", fixtures) {
            override fun getTextFor(value: Fixture): String = value.displayName
            override fun getIconFor(value: Fixture) = CajetaFileType.icon

            override fun onChosen(selected: Fixture, finalChoice: Boolean): PopupStep<*>? {
                openAndType(project, selected, delayMs)
                return PopupStep.FINAL_CHOICE
            }
        }
        return JBPopupFactory.getInstance().createListPopup(step)
    }

    private fun openAndType(project: Project, fixture: Fixture, delayMs: Int) {
        val scratch = makeScratchFile(fixture)
        val editor = FileEditorManager.getInstance(project)
            .openTextEditor(OpenFileDescriptor(project, scratch), true)
            ?: return
        FixtureTyper(project, editor, fixture, delayMs).runUnderProgress()
    }

    private fun makeScratchFile(fixture: Fixture): VirtualFile {
        // Use a LightVirtualFile so we don't pollute the user's
        // project. The file gets a name derived from the fixture so
        // it's easy to find in tabs/recent files. LightVirtualFile's
        // (name, fileType, content) constructor sets the file type
        // explicitly, so no extra hint to LoadTextUtil is needed.
        val baseName = fixture.path.fileName.toString().removeSuffix(".cajeta")
        val name = "harness-$baseName-${System.currentTimeMillis()}.cajeta"
        return LightVirtualFile(name, CajetaFileType, "")
    }
}
