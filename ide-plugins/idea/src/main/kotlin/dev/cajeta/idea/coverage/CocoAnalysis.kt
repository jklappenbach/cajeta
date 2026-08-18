package dev.cajeta.idea.coverage

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.components.Service
import com.intellij.openapi.project.Project
import dev.cajeta.idea.buildtool.CajetaManifest
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicReference

/**
 * The last loaded run, and what the coco views derive from it.
 *
 * A service rather than panel state, because the analysis outlives any one tab
 * and Units 7–8 will read the same run. Listeners let a tab that opens after the
 * load still see it, which is the normal order: results land, then someone goes
 * looking.
 */
@Service(Service.Level.PROJECT)
class CocoAnalysis(private val project: Project) {

    private val current = AtomicReference<CocoCoverage?>(null)
    private val findings = AtomicReference<List<UncoveredMethod>>(emptyList())
    private val listeners = CopyOnWriteArrayList<(List<UncoveredMethod>) -> Unit>()

    val coverage: CocoCoverage? get() = current.get()
    val deadCode: List<UncoveredMethod> get() = findings.get()

    fun addListener(listener: (List<UncoveredMethod>) -> Unit) {
        listeners += listener
        listener(findings.get())
    }

    /**
     * Recompute from a freshly loaded run. Called off the EDT — it queries the
     * index, which must not run on it.
     */
    fun update(coverage: CocoCoverage) {
        current.set(coverage)
        val classified = CocoDeadCode.classify(
            coverage = coverage,
            edges = CocoXrefEdges(project),
            entryKeys = entryKeys(),
            indexReason = CocoXrefEdges.unavailableReason(project),
        )
        findings.set(classified)
        ApplicationManager.getApplication().invokeLater {
            if (!project.isDisposed) listeners.forEach { it(classified) }
        }
    }

    /**
     * The declared entry point, keyed coco's way.
     *
     * Arity is unknown from the manifest — it names `pkg.Class.main`, not a
     * signature — so every arity that appears in the run is seeded. coco does
     * the same ("the entry (any arity)"), and the direction is safe: extra roots
     * can only move a verdict away from "dead".
     */
    private fun entryKeys(): Set<String> {
        val entry = CajetaManifest.buildSettings(project).entryMethod ?: return emptySet()
        val owner = entry.substringBeforeLast('.', "")
        val name = entry.substringAfterLast('.')
        if (owner.isEmpty() || name.isEmpty()) return emptySet()
        return (0..MAX_ENTRY_ARITY).map { "$owner::$name/$it" }.toSet()
    }

    companion object {
        /** `main()` and `main(String[])` are the real cases; the rest are free. */
        private const val MAX_ENTRY_ARITY = 4

        fun getInstance(project: Project): CocoAnalysis =
            project.getService(CocoAnalysis::class.java)
    }
}
