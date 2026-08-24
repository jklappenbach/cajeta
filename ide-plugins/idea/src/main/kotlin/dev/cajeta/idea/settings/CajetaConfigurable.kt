package dev.cajeta.idea.settings

import com.intellij.openapi.options.Configurable
import com.intellij.openapi.ui.ComboBox
import com.intellij.ui.JBColor
import com.intellij.ui.components.JBCheckBox
import com.intellij.ui.components.JBLabel
import com.intellij.ui.components.JBTextArea
import com.intellij.ui.components.JBTextField
import com.intellij.util.ui.FormBuilder
import dev.cajeta.idea.buildtool.BuildToolPathValidator
import dev.cajeta.idea.debugger.MemoryFacetLegend
import javax.swing.JComponent
import javax.swing.JPanel
import javax.swing.event.DocumentEvent
import javax.swing.event.DocumentListener

class CajetaConfigurable : Configurable {

    private var compilerPathField: JBTextField? = null
    private var renderMarkdownCheck: JBCheckBox? = null
    private var markdownSurfaceCombo: ComboBox<String>? = null
    private var fixturesPathField: JBTextField? = null
    private var typingDelayField: JBTextField? = null
    // CP7-5: memory-facet visualization toggles (FR-7.3).
    private var facetsVariablesCheck: JBCheckBox? = null
    private var facetsGutterCheck: JBCheckBox? = null
    private var facetsInlineCheck: JBCheckBox? = null
    // variable-inspection §3.1.4: rows per page when expanding an aggregate.
    private var debugPageSizeField: JBTextField? = null
    // Build-tool tool window settings (spec §14).
    private var buildToolPathField: JBTextField? = null
    private var buildToolPathProblem: JBLabel? = null
    private var autoReloadCombo: ComboBox<String>? = null
    private var defaultProfileField: JBTextField? = null
    private var defaultFlavorField: JBTextField? = null
    private var jsonlStructuredCheck: JBCheckBox? = null
    private var jsonlLevelField: JBTextField? = null
    private var buildInBuildWindowCheck: JBCheckBox? = null
    // lint-server §5: route per-edit lint through the warm daemon.
    private var lintServerCheck: JBCheckBox? = null
    private var panel: JPanel? = null

    override fun getDisplayName(): String = "Cajeta"

    override fun createComponent(): JComponent {
        val settings = CajetaSettings.instance
        val pathField = JBTextField(settings.compilerPath, 40).also { compilerPathField = it }
        val renderCheck = JBCheckBox(
            "Render markdown in comments (Obsidian-style)",
            settings.renderMarkdownInComments,
        ).also { renderMarkdownCheck = it }
        val surfaceCombo = ComboBox(
            arrayOf(CajetaSettings.MARKDOWN_SURFACE_SWING, CajetaSettings.MARKDOWN_SURFACE_JCEF),
        ).also { it.selectedItem = settings.markdownRenderSurface; markdownSurfaceCombo = it }
        val fixturesField = JBTextField(settings.testFixturesPath, 40).also { fixturesPathField = it }
        val delayField = JBTextField(settings.testTypingDelayMs.toString(), 6).also { typingDelayField = it }

        val varsCheck = JBCheckBox(
            "Show ownership/allocation/lifetime in the Variables view",
            settings.showFacetsInVariables,
        ).also { facetsVariablesCheck = it }
        val gutterCheck = JBCheckBox(
            "Show memory-facet icons in the editor gutter while debugging",
            settings.showFacetsInGutter,
        ).also { facetsGutterCheck = it }
        val inlineCheck = JBCheckBox(
            "Show inline memory-facet hints on the current line while debugging",
            settings.showFacetsInline,
        ).also { facetsInlineCheck = it }
        val pageSizeField = JBTextField(settings.debugPageSize.toString(), 6)
            .also { debugPageSizeField = it }
        val legend = JBTextArea(MemoryFacetLegend.text()).apply {
            isEditable = false
            isOpaque = false
            lineWrap = false
        }

        // Build-tool tool window controls (spec §14).
        val btPath = JBTextField(settings.buildToolPath, 40).also { buildToolPathField = it }
        val btProblem = JBLabel().apply { foreground = JBColor.RED }.also { buildToolPathProblem = it }
        // An invalid path is an error; a valid path that is too old for coco is
        // a WARNING in a different colour. Conflating them would either shout
        // about a working toolchain or bury the one line that explains an
        // otherwise unattributable `value=0x3` stack from a coverage run.
        val btVersionNote = JBLabel().apply { foreground = JBColor.ORANGE }
        fun refreshBtProblem() {
            val invalid = BuildToolPathValidator.problem(btPath.text)
            btProblem.text = invalid ?: ""
            btVersionNote.text =
                if (invalid != null) ""
                else BuildToolPathValidator.coverageFloorWarningFor(btPath.text) ?: ""
        }
        refreshBtProblem()
        btPath.document.addDocumentListener(object : DocumentListener {
            override fun insertUpdate(e: DocumentEvent) = refreshBtProblem()
            override fun removeUpdate(e: DocumentEvent) = refreshBtProblem()
            override fun changedUpdate(e: DocumentEvent) = refreshBtProblem()
        })
        val reloadCombo = ComboBox(
            arrayOf(
                CajetaSettings.AUTO_RELOAD_PROMPT,
                CajetaSettings.AUTO_RELOAD_ALWAYS,
                CajetaSettings.AUTO_RELOAD_NEVER,
            ),
        ).also { it.selectedItem = settings.buildAutoReload; autoReloadCombo = it }
        val profileField = JBTextField(settings.defaultProfile, 20).also { defaultProfileField = it }
        val flavorField = JBTextField(settings.defaultFlavor, 20).also { defaultFlavorField = it }
        val jsonlStructured = JBCheckBox(
            "Open JSONL views in structured mode by default",
            settings.jsonlDefaultStructured,
        ).also { jsonlStructuredCheck = it }
        val jsonlLevel = JBTextField(settings.jsonlDefaultLevel, 12).also { jsonlLevelField = it }
        val buildInBuildWindow = JBCheckBox(
            "Run build tasks in the Build tool window (off = Run window)",
            settings.buildTasksInBuildWindow,
        ).also { buildInBuildWindowCheck = it }
        val lintServer = JBCheckBox(
            "Use the warm lint server (off = a fresh compile per edit; much slower)",
            settings.useLintServer,
        ).also { lintServerCheck = it }

        panel = FormBuilder.createFormBuilder()
            .addLabeledComponent(JBLabel("cajetac binary:"), pathField, 1, false)
            .addComponent(renderCheck, 1)
            .addLabeledComponent(JBLabel("Markdown render surface (jcef = experimental, full CSS):"), surfaceCombo, 1, false)
            .addSeparator()
            .addComponent(JBLabel("Debugger — memory ownership visualization:"), 1)
            .addComponent(varsCheck, 1)
            .addComponent(gutterCheck, 1)
            .addComponent(inlineCheck, 1)
            .addLabeledComponent(
                JBLabel("Variables expansion page size (rows per page):"),
                pageSizeField, 1, false,
            )
            .addComponent(JBLabel("Legend:"), 1)
            .addComponent(legend, 1)
            .addSeparator()
            .addLabeledComponent(JBLabel("Test fixtures path:"), fixturesField, 1, false)
            .addLabeledComponent(JBLabel("Typing-harness delay (ms):"), delayField, 1, false)
            .addComponent(lintServer, 1)
            .addSeparator()
            .addComponent(JBLabel("Build tool:"), 1)
            .addLabeledComponent(JBLabel("Build-tool path:"), btPath, 1, false)
            .addComponent(btProblem, 1)
            .addComponent(btVersionNote, 1)
            .addLabeledComponent(JBLabel("Auto-reload on manifest change:"), reloadCombo, 1, false)
            .addLabeledComponent(JBLabel("Default profile:"), profileField, 1, false)
            .addLabeledComponent(JBLabel("Default flavor:"), flavorField, 1, false)
            .addComponent(jsonlStructured, 1)
            .addLabeledComponent(JBLabel("JSONL default level filter:"), jsonlLevel, 1, false)
            .addComponent(buildInBuildWindow, 1)
            .addComponentFillVertically(JPanel(), 0)
            .panel
        return panel!!
    }

    override fun isModified(): Boolean {
        val s = CajetaSettings.instance
        return (compilerPathField?.text ?: "") != s.compilerPath ||
            (renderMarkdownCheck?.isSelected ?: true) != s.renderMarkdownInComments ||
            (markdownSurfaceCombo?.selectedItem as? String ?: s.markdownRenderSurface) != s.markdownRenderSurface ||
            (fixturesPathField?.text ?: "") != s.testFixturesPath ||
            (typingDelayField?.text?.toIntOrNull() ?: s.testTypingDelayMs) != s.testTypingDelayMs ||
            (facetsVariablesCheck?.isSelected ?: true) != s.showFacetsInVariables ||
            (facetsGutterCheck?.isSelected ?: true) != s.showFacetsInGutter ||
            (facetsInlineCheck?.isSelected ?: true) != s.showFacetsInline ||
            (buildToolPathField?.text ?: "") != s.buildToolPath ||
            (autoReloadCombo?.selectedItem as? String ?: s.buildAutoReload) != s.buildAutoReload ||
            (defaultProfileField?.text ?: "") != s.defaultProfile ||
            (defaultFlavorField?.text ?: "") != s.defaultFlavor ||
            (jsonlStructuredCheck?.isSelected ?: true) != s.jsonlDefaultStructured ||
            (jsonlLevelField?.text ?: "") != s.jsonlDefaultLevel ||
            (buildInBuildWindowCheck?.isSelected ?: true) != s.buildTasksInBuildWindow ||
            (lintServerCheck?.isSelected ?: true) != s.useLintServer ||
            (debugPageSizeField?.text?.toIntOrNull() ?: s.debugPageSize) != s.debugPageSize
    }

    override fun apply() {
        val s = CajetaSettings.instance
        s.compilerPath = compilerPathField?.text?.trim().orEmpty()
        s.renderMarkdownInComments = renderMarkdownCheck?.isSelected ?: true
        s.markdownRenderSurface = markdownSurfaceCombo?.selectedItem as? String ?: CajetaSettings.MARKDOWN_SURFACE_SWING
        s.testFixturesPath = fixturesPathField?.text?.trim().orEmpty()
        typingDelayField?.text?.toIntOrNull()?.let {
            s.testTypingDelayMs = it.coerceIn(1, 5000)
        }
        s.showFacetsInVariables = facetsVariablesCheck?.isSelected ?: true
        s.showFacetsInGutter = facetsGutterCheck?.isSelected ?: true
        s.showFacetsInline = facetsInlineCheck?.isSelected ?: true
        s.buildToolPath = buildToolPathField?.text?.trim().orEmpty()
        s.buildAutoReload = autoReloadCombo?.selectedItem as? String ?: CajetaSettings.AUTO_RELOAD_PROMPT
        s.defaultProfile = defaultProfileField?.text?.trim().orEmpty()
        s.defaultFlavor = defaultFlavorField?.text?.trim().orEmpty()
        s.jsonlDefaultStructured = jsonlStructuredCheck?.isSelected ?: true
        s.jsonlDefaultLevel = jsonlLevelField?.text?.trim().orEmpty()
        s.buildTasksInBuildWindow = buildInBuildWindowCheck?.isSelected ?: true
        s.useLintServer = lintServerCheck?.isSelected ?: true
        // An unparseable or out-of-range entry keeps the stored value rather
        // than writing a page size the server would reject — the same shape as
        // the typing-delay field above.
        debugPageSizeField?.text?.toIntOrNull()?.let {
            s.debugPageSize = it.coerceIn(
                CajetaSettings.MIN_DEBUG_PAGE_SIZE,
                CajetaSettings.MAX_DEBUG_PAGE_SIZE,
            )
        }
    }

    override fun reset() {
        val s = CajetaSettings.instance
        compilerPathField?.text = s.compilerPath
        renderMarkdownCheck?.isSelected = s.renderMarkdownInComments
        markdownSurfaceCombo?.selectedItem = s.markdownRenderSurface
        fixturesPathField?.text = s.testFixturesPath
        typingDelayField?.text = s.testTypingDelayMs.toString()
        facetsVariablesCheck?.isSelected = s.showFacetsInVariables
        facetsGutterCheck?.isSelected = s.showFacetsInGutter
        facetsInlineCheck?.isSelected = s.showFacetsInline
        buildToolPathField?.text = s.buildToolPath
        autoReloadCombo?.selectedItem = s.buildAutoReload
        defaultProfileField?.text = s.defaultProfile
        defaultFlavorField?.text = s.defaultFlavor
        jsonlStructuredCheck?.isSelected = s.jsonlDefaultStructured
        jsonlLevelField?.text = s.jsonlDefaultLevel
        buildInBuildWindowCheck?.isSelected = s.buildTasksInBuildWindow
        lintServerCheck?.isSelected = s.useLintServer
        debugPageSizeField?.text = s.debugPageSize.toString()
    }

    override fun disposeUIResources() {
        compilerPathField = null
        renderMarkdownCheck = null
        markdownSurfaceCombo = null
        fixturesPathField = null
        typingDelayField = null
        facetsVariablesCheck = null
        facetsGutterCheck = null
        facetsInlineCheck = null
        buildToolPathField = null
        buildToolPathProblem = null
        autoReloadCombo = null
        defaultProfileField = null
        defaultFlavorField = null
        jsonlStructuredCheck = null
        jsonlLevelField = null
        buildInBuildWindowCheck = null
        lintServerCheck = null
        debugPageSizeField = null
        panel = null
    }
}
