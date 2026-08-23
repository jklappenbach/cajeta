package dev.cajeta.idea.xref

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.project.DumbService
import com.intellij.openapi.project.IndexNotReadyException
import com.intellij.openapi.project.Project
import com.intellij.psi.search.GlobalSearchScope
import com.intellij.util.indexing.FileBasedIndex
import dev.cajeta.idea.debugger.Json

/**
 * Query facade over [CajetaXrefIndex] (ide-symbol-index Unit 6). Every answer
 * is what the compiler exported — never computed here. Records come back as
 * raw [Json.Obj]s in the schema's shape; presentation layers shape them.
 */
object XrefQuery {

    /** Declaration records for an exact FQN. */
    fun declarationsOf(project: Project, fqn: String): List<Json.Obj> =
        records(project, "fqn:$fqn")

    /** Declared FQNs whose simple (last-segment) name matches. */
    fun fqnsForSimpleName(project: Project, simpleName: String): List<String> =
        rawValues(project, "name:$simpleName").distinct()

    /** Declaration records for an exact overloadKey (call targets, Unit 7). */
    fun declarationsForOverloadKey(project: Project, overloadKey: String): List<Json.Obj> =
        records(project, "key:$overloadKey")

    /** Inheritance records naming `parentFqn` as parent (child = subtype). */
    fun subtypesOf(project: Project, parentFqn: String): List<Json.Obj> =
        records(project, "sub:$parentFqn")

    /** Inheritance records naming what [childFqn] extends or implements. */
    fun supertypesOf(project: Project, childFqn: String): List<Json.Obj> =
        records(project, "super:$childFqn")

    /** Call records to exactly this overloadKey — one overload, no conflation. */
    fun callersOf(project: Project, calleeOverloadKey: String): List<Json.Obj> =
        records(project, "call:$calleeOverloadKey")

    /** Reference + call records sited in a root-relative source file. */
    fun usesIn(project: Project, sourceRelPath: String): List<Json.Obj> =
        records(project, "use:$sourceRelPath")

    /** Reference records targeting an FQN (find-usages direction). */
    fun usagesOf(project: Project, targetFqn: String): List<Json.Obj> =
        records(project, "uses:$targetFqn")

    /** Override records for methods overriding this overloadKey. */
    fun overridersOf(project: Project, overloadKey: String): List<Json.Obj> =
        records(project, "ovr:$overloadKey")

    /** Override records telling what this overloadKey itself overrides. */
    fun overriddenBy(project: Project, overloadKey: String): List<Json.Obj> =
        records(project, "ovrof:$overloadKey")

    private fun records(project: Project, key: String): List<Json.Obj> =
        rawValues(project, key).mapNotNull {
            try { Json.parse(it) as? Json.Obj } catch (e: Exception) { null }
        }

    /**
     * Index access requires a READ ACTION, and this facade is reached from three
     * kinds of caller: the EDT (gutters, navigation), an existing read action
     * (inspections), and a plain pooled thread (the coverage load, which
     * deliberately classifies off the EDT). The last of those had no read action
     * and threw straight out of `FileBasedIndexImpl.ensureUpToDate`, aborting the
     * whole coverage load — the query is the shared point, so the read action
     * belongs here rather than at each call site.
     *
     * `runReadActionInSmartMode` also waits out indexing. Querying during dumb
     * mode throws `IndexNotReadyException`, which is not a bug and must not read
     * as one.
     */
    private fun rawValues(project: Project, key: String): List<String> {
        if (project.isDisposed) return emptyList()
        val app = ApplicationManager.getApplication()
        return try {
            if (app.isReadAccessAllowed) {
                query(project, key)
            } else {
                DumbService.getInstance(project)
                    .runReadActionInSmartMode<List<String>> { query(project, key) }
            }
        } catch (e: IndexNotReadyException) {
            // Reached only when a caller already held a read action during
            // indexing, so waiting for smart mode was not an option. Degrading is
            // correct; degrading SILENTLY is not — an empty xref turns into
            // "nothing is reachable", which downstream reads as dead code.
            LOG.warn("coco/xref: index not ready for '$key'; answering empty", e)
            emptyList()
        }
    }

    private fun query(project: Project, key: String): List<String> {
        val out = mutableListOf<String>()
        FileBasedIndex.getInstance().processValues(
            CajetaXrefIndex.NAME, key, null,
            { _, value -> out.addAll(value.lineSequence()); true },
            GlobalSearchScope.allScope(project))
        return out
    }

    private val LOG = com.intellij.openapi.diagnostic.logger<XrefQuery>()
}
