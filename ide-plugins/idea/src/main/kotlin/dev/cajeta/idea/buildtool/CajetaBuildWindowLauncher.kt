package dev.cajeta.idea.buildtool

import com.intellij.build.BuildViewManager
import com.intellij.icons.AllIcons
import com.intellij.openapi.actionSystem.AnAction
import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.project.Project
import dev.cajeta.idea.settings.CajetaSettings
import java.io.File

/**
 * Production build-routed launch (spec §3, §5): spawns `cajeta <task> …` and
 * drives the project's `BuildViewManager` (the native Build tool window) through
 * [CajetaBuildBridge] on a pooled thread (off the EDT). Attaches Stop + Restart
 * toolbar actions to the build and registers a cancel hook with [BuildRunTracker]
 * so the tool-window Stop covers it too (spec §5.1–5.4).
 */
object CajetaBuildWindowLauncher {

    fun run(project: Project, spec: TaskRunSpec, restart: () -> Unit) {
        val buildToolPath = CajetaSettings.instance.buildToolPath
        val title = "cajeta ${spec.task}"
        val workDir = spec.manifestPath?.takeIf { it.isNotBlank() }?.let { File(it).parent }
            ?: project.basePath ?: "."
        val argv = listOf(buildToolPath) + CajetaCommandLine.runArgv(spec)

        val process = ProcessBuildTaskProcess(argv, workDir)
        val parser = BuildProblemParser()
        val lineParser = LineParser { line, pid -> parser.feed(line)?.toParsed(pid) }

        val tracker = BuildRunTracker.getInstance(project)
        val cancelable = BuildRunTracker.Cancelable { process.cancel() }
        val stopAction = object : AnAction("Stop", "Stop the cajeta build", AllIcons.Actions.Suspend) {
            override fun actionPerformed(e: AnActionEvent) { process.cancel() }
        }
        val restartAction = object : AnAction("Restart", "Re-run the cajeta build", AllIcons.Actions.Restart) {
            override fun actionPerformed(e: AnActionEvent) { restart() }
        }

        val viewManager = project.getService(BuildViewManager::class.java)
        val buildId = Any()
        tracker.registerCancelable(cancelable)
        ApplicationManager.getApplication().executeOnPooledThread {
            try {
                CajetaBuildBridge.execute(
                    listener = viewManager,
                    buildId = buildId,
                    title = title,
                    workDir = workDir,
                    startTime = System.currentTimeMillis(),
                    preflight = { BuildToolPathValidator.problem(buildToolPath) },
                    process = process,
                    lineParser = lineParser,
                    decorate = { it.withAction(stopAction).withRestartAction(restartAction) },
                )
            } finally {
                tracker.unregisterCancelable(cancelable)
            }
        }
    }
}
