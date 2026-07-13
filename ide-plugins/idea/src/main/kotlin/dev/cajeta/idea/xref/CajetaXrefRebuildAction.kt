package dev.cajeta.idea.xref

import com.intellij.openapi.actionSystem.AnAction
import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.progress.ProgressIndicator
import com.intellij.openapi.progress.Task
import com.intellij.openapi.project.Project
import dev.cajeta.idea.settings.CajetaSettings
import java.io.File
import java.nio.file.Files
import java.util.concurrent.TimeUnit

/**
 * Cold / rebuild indexing (ide-symbol-index Unit 9, 9.2.4): run the whole-root
 * export (`--lint <root> --emit-xref=<path>`, Unit 3) in the BACKGROUND with
 * progress, ingest the document into per-file shards, and drive the freshness
 * state machine. Typing is never blocked — this is a Task.Backgroundable, and
 * navigation keeps answering from the previous shards until the new ones land
 * (9.1.1).
 */
class CajetaXrefRebuildAction : AnAction("Rebuild Cajeta Xref Index") {

    override fun actionPerformed(e: AnActionEvent) {
        val project = e.project ?: return
        rebuild(project)
    }

    companion object {
        private val log = Logger.getInstance(CajetaXrefRebuildAction::class.java)

        fun rebuild(project: Project) {
            val freshness = CajetaXrefFreshness.getInstance(project)
            val compilerPath = CajetaSettings.instance.compilerPath
            if (compilerPath.isBlank() || !File(compilerPath).canExecute()) {
                freshness.updateFromLint(compilerConfigured = false,
                    stream = dev.cajeta.idea.lint.XrefStream.EMPTY)
                return
            }
            val base = project.basePath ?: return
            // The compiler wants the SOURCE root; prefer the conventional
            // layout, fall back to the project base.
            val srcRoot = File(base, "src/main/cajeta")
                .takeIf { it.isDirectory }?.path ?: base

            freshness.refreshStarted()
            object : Task.Backgroundable(project, "Rebuilding Cajeta xref index", true) {
                override fun run(indicator: ProgressIndicator) {
                    indicator.isIndeterminate = true
                    try {
                        val out = Files.createTempFile("cajeta-xref-", ".json")
                        val p = ProcessBuilder(compilerPath, "--lint", srcRoot,
                                "--emit-xref=$out", "--diag-format=json")
                            .redirectErrorStream(false).start()
                        p.inputStream.bufferedReader().readText()
                        p.errorStream.bufferedReader().readText()
                        if (!p.waitFor(600, TimeUnit.SECONDS)) {
                            p.destroyForcibly()
                            freshness.refreshFailed("whole-root export timed out")
                            return
                        }
                        val doc = String(Files.readAllBytes(out))
                        Files.deleteIfExists(out)
                        if (!CajetaXrefShards.ingestDocument(project, doc)) {
                            freshness.refreshFailed(
                                "export refused (unreadable or unknown schema major)")
                            return
                        }
                        freshness.refreshSucceeded()
                    } catch (t: Throwable) {
                        log.warn("xref rebuild failed", t)
                        freshness.refreshFailed(t.message ?: "rebuild failed")
                    }
                }
            }.queue()
        }
    }
}
