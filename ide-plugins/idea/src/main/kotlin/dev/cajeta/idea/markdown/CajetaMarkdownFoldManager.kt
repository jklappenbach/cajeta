package dev.cajeta.idea.markdown

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.application.invokeLater
import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.editor.CustomFoldRegion
import com.intellij.openapi.editor.Editor
import com.intellij.openapi.editor.EditorFactory
import com.intellij.openapi.editor.FoldRegion
import com.intellij.openapi.editor.Inlay
import com.intellij.openapi.editor.event.DocumentEvent
import com.intellij.openapi.editor.event.DocumentListener
import com.intellij.openapi.editor.event.EditorFactoryEvent
import com.intellij.openapi.editor.event.EditorFactoryListener
import com.intellij.openapi.editor.ex.FoldingModelEx
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.startup.ProjectActivity
import com.intellij.openapi.util.Disposer
import com.intellij.openapi.util.Key
import com.intellij.util.Alarm
import dev.cajeta.idea.CajetaFileType
import dev.cajeta.idea.settings.CajetaSettings

/**
 * On each Cajeta editor's creation, scans the document for comment
 * regions and installs a CustomFoldRegion for each — collapsed by
 * default, painted by MarkdownFoldRenderer. The caret listener (see
 * CommentCaretListener) handles expand-on-caret-enter behavior.
 *
 * Block comments each get their own region. Contiguous LINE_COMMENT
 * runs are grouped into a single region — blank source lines or
 * non-comment tokens terminate a run.
 */
class InstallMarkdownFoldManager : ProjectActivity {

    override suspend fun execute(project: Project) {
        EditorFactory.getInstance()
            .addEditorFactoryListener(MarkdownFoldEditorListener, project)

        // Also run against editors that are already open at the time
        // the activity fires (typical at project reopen).
        invokeLater {
            for (editor in EditorFactory.getInstance().allEditors) {
                MarkdownFoldEditorListener.install(editor)
            }
        }
    }
}

internal object MarkdownFoldEditorListener : EditorFactoryListener {

    private val log = Logger.getInstance(MarkdownFoldEditorListener::class.java)

    override fun editorCreated(event: EditorFactoryEvent) {
        install(event.editor)
    }

    fun install(editor: Editor) {
        if (!isCajetaEditor(editor)) return
        if (!CajetaSettings.instance.renderMarkdownInComments) return

        ApplicationManager.getApplication().invokeLater {
            try {
                installFoldRegions(editor)
                installDocumentRefreshListener(editor)
            } catch (e: Throwable) {
                log.warn("Failed to install markdown fold regions", e)
            }
        }
    }

    /**
     * Refreshes fold regions when the document changes, so that
     * editing a comment while expanded (or adding/removing comment
     * lines) results in the rendered HTML reflecting the new source.
     * Debounced ~400 ms so typing doesn't thrash the folding model.
     */
    private fun installDocumentRefreshListener(editor: Editor) {
        if (editor.getUserData(REFRESH_LISTENER_KEY) != null) return
        val alarm = Alarm(Alarm.ThreadToUse.SWING_THREAD, editor.project ?: return)
        val listener = object : DocumentListener {
            override fun documentChanged(event: DocumentEvent) {
                if (!CajetaSettings.instance.renderMarkdownInComments) return
                alarm.cancelAllRequests()
                alarm.addRequest({
                    try {
                        refresh(editor)
                    } catch (e: Throwable) {
                        log.warn("Failed to refresh markdown fold regions", e)
                    }
                }, REFRESH_DEBOUNCE_MS)
            }
        }
        editor.document.addDocumentListener(listener)
        editor.putUserData(REFRESH_LISTENER_KEY, listener)
    }

    /**
     * Tears down all current fold regions and inlays for this editor,
     * then re-runs installFoldRegions with the current document text.
     * Skips refresh if the caret is inside an expanded whole-line
     * block — that means the user is mid-edit and we shouldn't yank
     * the source out from under them.
     */
    private fun refresh(editor: Editor) {
        val caretOffset = editor.caretModel.offset

        val activeBlock = findBlockAt(editor, caretOffset)
        if (activeBlock != null && !activeBlock.isCollapsed) {
            // Caret is inside a block the user has expanded for editing.
            // Don't tear down — wait until they move away.
            log.debug("refresh skipped: caret inside expanded block ${activeBlock.startOffset}-${activeBlock.endOffset}")
            return
        }

        teardownAll(editor)
        // Rebuild from the current document.
        installFoldRegions(editor)
    }

    /**
     * Removes all installed markdown fold state for this editor — whole-line
     * CustomFoldRegions and trailing folds + their inlays — without rebuilding.
     * Shared by refresh() (which rebuilds after) and uninstall() (which does
     * not). Unconditional: callers that must preserve a caret-in-block edit
     * (refresh) guard before invoking this.
     */
    private fun teardownAll(editor: Editor) {
        val foldingModel = editor.foldingModel as? FoldingModelEx ?: return

        // Tear down whole-line block state + their CustomFoldRegions.
        val blocks = wholeLineBlocksFor(editor)
        foldingModel.runBatchFoldingOperation {
            for (block in blocks) {
                block.customRegion?.let {
                    if (it.isValid) foldingModel.removeFoldRegion(it)
                }
                block.customRegion = null
            }
        }
        blocks.clear()

        // Tear down trailing folds + their inlays.
        val trailing = foldDataFor(editor)
        foldingModel.runBatchFoldingOperation {
            for ((region, data) in trailing) {
                data.inlay?.let { if (it.isValid) Disposer.dispose(it) }
                if (region.isValid) foldingModel.removeFoldRegion(region)
            }
        }
        trailing.clear()
    }

    /**
     * Removes markdown rendering from this editor and stops its refresh
     * listener — used when the feature is toggled off (W3b). Unlike refresh(),
     * this tears down unconditionally (even a block the caret sits in), since
     * the user asked for raw source everywhere.
     */
    fun uninstall(editor: Editor) {
        if (!isCajetaEditor(editor)) return
        ApplicationManager.getApplication().invokeLater {
            try {
                teardownAll(editor)
                editor.getUserData(REFRESH_LISTENER_KEY)?.let {
                    editor.document.removeDocumentListener(it)
                    editor.putUserData(REFRESH_LISTENER_KEY, null)
                }
            } catch (e: Throwable) {
                log.warn("Failed to uninstall markdown fold regions", e)
            }
        }
    }

    /**
     * Applies the current [CajetaSettings.renderMarkdownInComments] state to
     * every open editor — installing rendering when on, tearing it down (revert
     * to raw source) when off. Driven by [ToggleMarkdownRenderingAction]; the
     * per-editor branch is [MarkdownRenderingToggle.opFor].
     */
    fun applyRenderingState() {
        val enabled = CajetaSettings.instance.renderMarkdownInComments
        ApplicationManager.getApplication().invokeLater {
            for (editor in EditorFactory.getInstance().allEditors) {
                when (MarkdownRenderingToggle.opFor(enabled)) {
                    MarkdownRenderingToggle.EditorOp.INSTALL -> install(editor)
                    MarkdownRenderingToggle.EditorOp.UNINSTALL -> uninstall(editor)
                }
            }
        }
    }

    private val REFRESH_LISTENER_KEY: Key<DocumentListener> =
        Key.create("dev.cajeta.idea.markdown.refreshListener")

    private const val REFRESH_DEBOUNCE_MS = 400

    private fun installFoldRegions(editor: Editor) {
        val foldingModel = editor.foldingModel as? FoldingModelEx ?: return
        val document = editor.document
        val text = document.charsSequence
        val runs = findCommentRuns(text)
        val trailing = findTrailingComments(text)
        val sideTable = foldDataFor(editor)
        val tabSize = editor.settings.getTabSize(editor.project)
        log.debug("installFoldRegions: ${runs.size} whole-line runs, ${trailing.size} trailing")

        val blockStates = mutableListOf<WholeLineBlockState>()
        foldingModel.runBatchFoldingOperation {
            for (run in runs) {
                val startLine = document.getLineNumber(run.startOffset)
                val endLine = document.getLineNumber(run.endOffset - 1) + 1
                if (endLine <= startLine) continue

                val existing = foldingModel.allFoldRegions.firstOrNull {
                    it.startOffset == run.startOffset && it.endOffset == run.endOffset
                }
                if (existing != null) continue

                val markdown = stripCommentMarkers(text.substring(run.startOffset, run.endOffset))
                if (markdown.isBlank()) continue

                val state = WholeLineBlockState(
                    startOffset = run.startOffset,
                    endOffset = run.endOffset,
                    startLine = startLine,
                    endLine = endLine - 1,
                    markdown = markdown,
                    indentColumns = indentColumnsAt(text, run.startOffset, tabSize),
                )
                collapseBlock(foldingModel, state)
                blockStates += state
            }
        }
        wholeLineBlocksFor(editor).apply { clear(); addAll(blockStates) }

        // Trailing comments: regular fold (empty placeholder) + inline
        // inlay painted at the `//` offset. Done outside the custom-
        // lines batch because the addFoldRegion calls below also need
        // their own batch.
        foldingModel.runBatchFoldingOperation {
            for (tr in trailing) {
                val existing = foldingModel.getFoldRegion(tr.startOffset, tr.endOffset)
                if (existing != null) continue

                val markdown = stripCommentMarkers(text.substring(tr.startOffset, tr.endOffset))
                if (markdown.isBlank()) continue

                val region = foldingModel.addFoldRegion(tr.startOffset, tr.endOffset, "") ?: continue
                region.isExpanded = false

                // addAfterLineEndElement places the inlay outside the
                // folded range (at end-of-visible-line) — addInlineElement
                // at the fold's start offset would render inside the
                // collapsed range and not be visible.
                val inlay = editor.inlayModel.addAfterLineEndElement(
                    tr.startOffset,
                    /* relatesToPrecedingText = */ true,
                    InlineMarkdownRenderer(markdown),
                ) ?: continue
                log.debug("trailing inlay installed at offset=${tr.startOffset} (${markdown.take(40)})")
                sideTable[region] = TrailingFoldData(markdown, inlay)
            }
        }
    }

    internal data class TrailingFoldData(val markdown: String, var inlay: Inlay<*>?)

    /**
     * State for one whole-line markdown block. CustomFoldRegion can't
     * be toggled via isExpanded — that setter is silently a no-op
     * (verified: expandedAfter=false even after setting it to true).
     * So we have to literally remove the region to "expand" and
     * re-add it to "collapse".
     */
    internal data class WholeLineBlockState(
        val startOffset: Int,
        val endOffset: Int,
        val startLine: Int,
        val endLine: Int,
        val markdown: String,
        /** Column the source comment starts at, so the rendered block can be
         *  indented to the scope of the code it documents. */
        val indentColumns: Int = 0,
        var customRegion: CustomFoldRegion? = null,
    ) {
        val isCollapsed: Boolean get() = customRegion?.isValid == true
    }

    /**
     * Visual column of [offset] within its line, expanding tabs to [tabSize].
     * Only ever called for whole-line comments, so everything between the line
     * start and [offset] is whitespace.
     */
    internal fun indentColumnsAt(text: CharSequence, offset: Int, tabSize: Int): Int {
        val lineStart = findLineStart(text, offset)
        var col = 0
        for (i in lineStart until offset) {
            col = if (text[i] == '\t') ((col / tabSize) + 1) * tabSize else col + 1
        }
        return col
    }

    private val WHOLE_LINE_BLOCKS_KEY: Key<MutableList<WholeLineBlockState>> =
        Key.create("dev.cajeta.idea.markdown.wholeLineBlocks")

    internal fun wholeLineBlocksFor(editor: Editor): MutableList<WholeLineBlockState> {
        var list = editor.getUserData(WHOLE_LINE_BLOCKS_KEY)
        if (list == null) {
            list = mutableListOf()
            editor.putUserData(WHOLE_LINE_BLOCKS_KEY, list)
        }
        return list
    }

    /** Find the block whose source-text range contains the given offset. */
    internal fun findBlockAt(editor: Editor, offset: Int): WholeLineBlockState? {
        return wholeLineBlocksFor(editor).firstOrNull {
            offset >= it.startOffset && offset <= it.endOffset
        }
    }

    /**
     * Find the block that owns a painted [region] — the identity lookup behind
     * geometric hit-testing.
     *
     * Clicks on a rendered block must NOT be resolved through
     * [findBlockAt]`(event.offset)`: a `CustomFoldRegion` paints a block far
     * taller than the collapsed text it stands in for, so a click inside it can
     * resolve to an offset outside the folded range and silently match nothing.
     * That is what made every indented block unclickable while the column-0 one
     * still worked. Hit-test on the region's painted bounds instead, then come
     * here for the state.
     */
    internal fun blockForRegion(editor: Editor, region: CustomFoldRegion): WholeLineBlockState? =
        wholeLineBlocksFor(editor).firstOrNull { it.customRegion === region }

    /** Expand by removing the CustomFoldRegion entirely; source text appears. */
    internal fun expandBlock(editor: Editor, state: WholeLineBlockState) {
        val foldingModel = editor.foldingModel as? FoldingModelEx ?: return
        val region = state.customRegion ?: return
        foldingModel.runBatchFoldingOperation {
            if (region.isValid) foldingModel.removeFoldRegion(region)
        }
        state.customRegion = null
        log.debug("expandBlock: removed CustomFoldRegion for lines ${state.startLine}-${state.endLine}")
    }

    /** Re-collapse by recreating the CustomFoldRegion; renderer paints again. */
    internal fun collapseBlock(editor: Editor, state: WholeLineBlockState) {
        if (state.customRegion?.isValid == true) return
        val foldingModel = editor.foldingModel as? FoldingModelEx ?: return
        foldingModel.runBatchFoldingOperation {
            collapseBlock(foldingModel, state)
        }
        log.debug("collapseBlock: re-added CustomFoldRegion for lines ${state.startLine}-${state.endLine}")
    }

    private fun collapseBlock(foldingModel: FoldingModelEx, state: WholeLineBlockState) {
        val renderer = MarkdownFoldRenderer(state.markdown, state.indentColumns)
        state.customRegion = foldingModel.addCustomLinesFolding(
            state.startLine, state.endLine, renderer,
        )
    }

    private val FOLD_DATA_KEY: Key<MutableMap<FoldRegion, TrailingFoldData>> =
        Key.create("dev.cajeta.idea.markdown.trailingFoldData")

    /** Per-editor map: trailing fold region → its paired inlay + source. */
    internal fun foldDataFor(editor: Editor): MutableMap<FoldRegion, TrailingFoldData> {
        var map = editor.getUserData(FOLD_DATA_KEY)
        if (map == null) {
            map = mutableMapOf()
            editor.putUserData(FOLD_DATA_KEY, map)
        }
        return map
    }

    /** Called by the caret listener when expanding a trailing fold. */
    internal fun onTrailingFoldExpanded(editor: Editor, region: FoldRegion) {
        val data = foldDataFor(editor)[region] ?: return
        data.inlay?.let { if (it.isValid) Disposer.dispose(it) }
        data.inlay = null
    }

    /** Called by the caret listener when re-collapsing a trailing fold. */
    internal fun onTrailingFoldCollapsed(editor: Editor, region: FoldRegion) {
        val data = foldDataFor(editor)[region] ?: return
        if (data.inlay?.isValid == true) return
        val newInlay = editor.inlayModel.addAfterLineEndElement(
            region.startOffset,
            /* relatesToPrecedingText = */ true,
            InlineMarkdownRenderer(data.markdown),
        )
        data.inlay = newInlay
    }

    internal fun isTrailingFold(editor: Editor, region: FoldRegion): Boolean =
        foldDataFor(editor).containsKey(region)

    private fun isCajetaEditor(editor: Editor): Boolean {
        val file = FileDocumentManager.getInstance().getFile(editor.document) ?: return false
        return file.fileType == CajetaFileType
    }

    /**
     * Lightweight comment-range scanner. Cajeta comments are simple
     * enough (line and block) that we do not need the parser here —
     * and avoiding it means fold regions can be installed before PSI
     * is built.
     */
    internal data class CommentRun(val startOffset: Int, val endOffset: Int)

    internal fun findCommentRuns(text: CharSequence): List<CommentRun> {
        val runs = mutableListOf<CommentRun>()
        var i = 0
        var currentLineRunStart = -1
        var currentLineRunEnd = -1
        val n = text.length

        fun flushLineRun() {
            if (currentLineRunStart >= 0 && currentLineRunEnd > currentLineRunStart) {
                runs += CommentRun(currentLineRunStart, currentLineRunEnd)
            }
            currentLineRunStart = -1
            currentLineRunEnd = -1
        }

        while (i < n) {
            val c = text[i]
            // String literals — skip without scanning, so `// in a string`
            // doesn't get treated as a comment.
            if (c == '"') {
                flushLineRun()
                i = skipString(text, i)
                continue
            }
            if (c == '\'') {
                flushLineRun()
                i = skipChar(text, i)
                continue
            }
            if (c == '/' && i + 1 < n) {
                val next = text[i + 1]
                if (next == '/') {
                    // Only fold whole-line comments. Markdown rendering
                    // on a trailing-comment line would have to repaint
                    // the entire line at the left margin (the
                    // CustomFoldRegion API is line-based), erasing the
                    // code that comes before. Skip trailing `//` and
                    // leave it as plain raw source.
                    val lineStart = findLineStart(text, i)
                    val prefix = text.subSequence(lineStart, i)
                    val isWholeLineComment = prefix.all { it == ' ' || it == '\t' }
                    if (!isWholeLineComment) {
                        // Trailing comment — terminates any open run and
                        // contributes nothing of its own.
                        flushLineRun()
                        var j = i + 2
                        while (j < n && text[j] != '\n') j++
                        i = if (j < n) j + 1 else j
                        continue
                    }
                    val commentStart = i
                    var j = i + 2
                    while (j < n && text[j] != '\n') j++
                    val lineEnd = j
                    if (currentLineRunStart < 0) {
                        currentLineRunStart = commentStart
                    } else {
                        // Continuation: only if no non-whitespace between
                        // the previous comment end and this comment start.
                        val between = text.subSequence(currentLineRunEnd, commentStart)
                        val onlyWs = between.all { it == ' ' || it == '\t' || it == '\n' || it == '\r' }
                        val newlines = between.count { it == '\n' }
                        if (!onlyWs || newlines > 1) {
                            flushLineRun()
                            currentLineRunStart = commentStart
                        }
                    }
                    currentLineRunEnd = lineEnd
                    i = if (j < n) j + 1 else j
                    continue
                }
                if (next == '*') {
                    // Same rule for block comments: only fold when the
                    // block starts at column 0 (modulo whitespace).
                    val lineStart = findLineStart(text, i)
                    val prefix = text.subSequence(lineStart, i)
                    val isWholeLineBlock = prefix.all { it == ' ' || it == '\t' }
                    var j = i + 2
                    while (j + 1 < n && !(text[j] == '*' && text[j + 1] == '/')) j++
                    val blockEnd = (j + 2).coerceAtMost(n)
                    if (isWholeLineBlock) {
                        flushLineRun()
                        runs += CommentRun(i, blockEnd)
                    } else {
                        // Trailing or mid-line block comment — leave raw.
                        flushLineRun()
                    }
                    i = blockEnd
                    continue
                }
            }
            // Anything else on a non-comment line breaks a `//` run.
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && currentLineRunStart >= 0) {
                flushLineRun()
            }
            i++
        }
        flushLineRun()
        return runs
    }

    /**
     * Finds single-line trailing `//` comments — those where there's
     * code on the same line before the `//`. Block comments mid-line
     * are not included; they're left as raw text.
     */
    internal fun findTrailingComments(text: CharSequence): List<CommentRun> {
        val out = mutableListOf<CommentRun>()
        var i = 0
        val n = text.length
        while (i < n) {
            val c = text[i]
            if (c == '"') { i = skipString(text, i); continue }
            if (c == '\'') { i = skipChar(text, i); continue }
            if (c == '/' && i + 1 < n && text[i + 1] == '/') {
                val lineStart = findLineStart(text, i)
                val prefix = text.subSequence(lineStart, i)
                val isWholeLine = prefix.all { it == ' ' || it == '\t' }
                var j = i + 2
                while (j < n && text[j] != '\n') j++
                if (!isWholeLine) {
                    out += CommentRun(i, j)
                }
                i = if (j < n) j + 1 else j
                continue
            }
            if (c == '/' && i + 1 < n && text[i + 1] == '*') {
                var j = i + 2
                while (j + 1 < n && !(text[j] == '*' && text[j + 1] == '/')) j++
                i = (j + 2).coerceAtMost(n)
                continue
            }
            i++
        }
        return out
    }

    private fun findLineStart(text: CharSequence, offset: Int): Int {
        var i = offset - 1
        while (i >= 0 && text[i] != '\n') i--
        return i + 1
    }

    private fun skipString(text: CharSequence, start: Int): Int {
        var i = start + 1
        val n = text.length
        while (i < n) {
            val c = text[i]
            if (c == '\\' && i + 1 < n) { i += 2; continue }
            if (c == '"') return i + 1
            if (c == '\n') return i + 1  // unterminated; recover at line end
            i++
        }
        return n
    }

    private fun skipChar(text: CharSequence, start: Int): Int {
        var i = start + 1
        val n = text.length
        while (i < n) {
            val c = text[i]
            if (c == '\\' && i + 1 < n) { i += 2; continue }
            if (c == '\'') return i + 1
            if (c == '\n') return i + 1
            i++
        }
        return n
    }

    private fun stripCommentMarkers(raw: String): String {
        return raw.lineSequence()
            .map { line ->
                val trimmed = line.trim()
                when {
                    trimmed.startsWith("//") -> trimmed.removePrefix("//").trimStart()
                    trimmed.startsWith("/*") -> trimmed
                        .removePrefix("/*").removeSuffix("*/").trim()
                    trimmed.startsWith("*") -> trimmed.removePrefix("*").trimStart()
                    else -> trimmed
                }
            }
            .joinToString("\n")
            .trim()
    }
}
