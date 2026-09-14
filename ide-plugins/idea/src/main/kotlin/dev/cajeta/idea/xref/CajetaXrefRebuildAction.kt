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
class CajetaXrefRebuildAction : AnAction("Rebuild Cajeta Index") {

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
            // The compiler wants the SOURCE root. Resolved through the shared
            // convention so this export and the debug run configuration always
            // describe the same tree (run-config-ergonomics 2.2.3 / spec 3.1.3).
            // EVERY source root, not just the conventional one
            // (lint-own-archive-classpath 7.2.2). Exporting only
            // `src/main/cajeta` left a project whose tests live elsewhere with no
            // index for them AT ALL — measured on cajeta-http, the shard named
            // `HttpSerializer` 587 times and `ServerTests` 0, so every import in
            // that file was dead because the file it clicks FROM was never
            // visited. Falls back to the conventional root when discovery finds
            // nothing (an empty or unreadable tree), so behaviour is unchanged
            // for a project with no sources yet.
            val srcRoots = dev.cajeta.idea.buildtool.CajetaRoots
                .sourceRootsOf(base)
                .ifEmpty { listOf(dev.cajeta.idea.buildtool.CajetaRoots.conventionalSourceRoot(base)) }

            freshness.refreshStarted()
            object : Task.Backgroundable(project, "Rebuilding Cajeta index", true) {
                override fun run(indicator: ProgressIndicator) {
                    indicator.isIndeterminate = true
                    try {
                        // The classpath is per PROJECT, not per root — resolve it
                        // once. Dependency .cja's carry their declarations so
                        // Ctrl-click into a dependency type has a target (§8.3.1),
                        // PLUS the project's own archive (6.2.1), without which
                        // exporting a test root yields nothing to click at all:
                        // measured on the two-root fixture, 0 records and 0
                        // mentions of the project's own type, against 45 with it.
                        val deps = CajetaSourceMountGlue.lintClasspath(compilerPath, base)
                        for ((i, srcRoot) in srcRoots.withIndex()) {
                            // Name the root: a multi-root rebuild is longer than a
                            // single-root one, and a silent pause reads as a hang
                            // (spec §8.8).
                            indicator.text =
                                if (srcRoots.size > 1)
                                    "Exporting ${File(srcRoot).name} (${i + 1}/${srcRoots.size})"
                                else "Exporting ${File(srcRoot).name}"

                            val out = Files.createTempFile("cajeta-xref-", ".json")
                            val argv = mutableListOf(compilerPath, "--lint", srcRoot,
                                "--emit-xref=$out", "--diag-format=json")
                            if (deps.isNotEmpty())
                                argv.add("--classpath=" + deps.joinToString(",") { it.toString() })
                            val p = ProcessBuilder(argv)
                                .redirectErrorStream(false).start()
                            p.inputStream.bufferedReader().readText()
                            p.errorStream.bufferedReader().readText()
                            if (!p.waitFor(600, TimeUnit.SECONDS)) {
                                p.destroyForcibly()
                                Files.deleteIfExists(out)
                                freshness.refreshFailed("export of $srcRoot timed out")
                                return
                            }
                            val doc = String(Files.readAllBytes(out))
                            Files.deleteIfExists(out)
                            // Shards are per SOURCE FILE, so roots write disjoint
                            // shards and these passes accumulate — no merge step,
                            // and no pass clobbers another (spec §8.5).
                            if (!CajetaXrefShards.ingestDocument(project, doc)) {
                                freshness.refreshFailed(
                                    "export of $srcRoot refused (unreadable or unknown schema major)")
                                return
                            }
                        }
                        freshness.refreshSucceeded()
                        // The manual rebuild is the developer's "fix everything"
                        // action — make sure dependency SOURCES are mounted too,
                        // not just their declarations exported (§8.3 first-open
                        // fix): shards without mounted files navigate nowhere.
                        CajetaSourceMounts.mountAll(project)
                    } catch (t: Throwable) {
                        log.warn("xref rebuild failed", t)
                        freshness.refreshFailed(t.message ?: "rebuild failed")
                    }
                }
            }.queue()
        }
    }
}
