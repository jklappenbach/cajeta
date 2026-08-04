package dev.cajeta.idea.xref

import com.intellij.openapi.project.Project
import com.intellij.openapi.startup.ProjectActivity
import dev.cajeta.idea.settings.CajetaSettings
import java.io.File

/**
 * Build the index once on project open (Julian, 2026-08-02).
 *
 * Freshness starts STALE — "no export ingested yet" — and only becomes FRESH
 * when an export is ingested. Until something triggers that, every feature
 * resting on the index behaves as though the project were unindexed: rename
 * reports a conflict it cannot avoid, hierarchy comes up thin, usages under-
 * report. Making the developer run Rebuild by hand before the IDE is useful
 * is a poor first impression of every one of those features.
 *
 * Deliberately cheap to skip: with no compiler configured there is nothing to
 * run, and an index already FRESH (a warm reopen that restored shards) is left
 * alone. The rebuild itself is the same backgroundable task the menu action
 * runs, so this adds a trigger and no new machinery.
 */
class CajetaXrefStartup : ProjectActivity {

    override suspend fun execute(project: Project) {
        val compiler = CajetaSettings.instance.compilerPath
        if (compiler.isBlank() || !File(compiler).canExecute()) return
        if (CajetaXrefFreshness.getInstance(project).safeForRefactoring()) return
        CajetaXrefRebuildAction.rebuild(project)
    }
}
