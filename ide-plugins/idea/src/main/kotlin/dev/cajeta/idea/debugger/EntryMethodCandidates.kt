package dev.cajeta.idea.debugger

import com.intellij.openapi.application.ReadAction
import com.intellij.openapi.project.Project
import dev.cajeta.idea.buildtool.CajetaManifest
import dev.cajeta.idea.buildtool.ManifestScan
import dev.cajeta.idea.xref.CajetaXrefFreshness
import dev.cajeta.idea.xref.XrefQuery
import java.io.File

/**
 * Entry-method candidates for the debug run configuration
 * (run-config-ergonomics §2, spec §2/§6).
 *
 * Two sources, in precedence order: what `cajeta.json` DECLARES, then what the
 * xref index has DISCOVERED. Declared wins — the manifest is the project saying
 * what its entry points are, and discovery only fills gaps (spec 1.4.4).
 *
 * The merge is pure so it can be tested without a Project or an index; the
 * Project-facing [forProject] just gathers the inputs.
 */
object EntryMethodCandidates {

    /** One offerable entry method. [declared] = came from the manifest. */
    data class Candidate(val fqn: String, val declared: Boolean)

    /**
     * The candidate list plus WHY it might be empty. "The index is unavailable"
     * and "the index is warm and found nothing" are different facts, and a UI
     * that renders them alike tells the developer their project has no entry
     * point when the truth is that nobody looked (spec 6.1.3).
     */
    data class Result(
        val candidates: List<Candidate>,
        val indexAvailable: Boolean,
    ) {
        val indexUnavailable: Boolean get() = !indexAvailable
        val noCandidatesFound: Boolean get() = indexAvailable && candidates.isEmpty()

        /** Null when there is something to show. */
        fun emptyMessage(): String? = when {
            candidates.isNotEmpty() -> null
            indexUnavailable ->
                "Xref index unavailable — declared entry methods only. " +
                    "Run Tools > Cajeta > Rebuild Cajeta Index."
            else -> "No static main methods found in this project."
        }
    }

    /** 3.1.6 / spec 2.1.5 — what gets persisted, whatever the developer typed. */
    fun persistedValueFor(text: String): String =
        CajetaManifest.normalizeEntryMethod(text.trim())

    /** Scan retries the dialog will attempt while the index warms. */
    const val MAX_SCAN_ATTEMPTS = 5

    /**
     * Whether the dialog should look again (first-open fix, 2026-07-30): the
     * first open races project indexing and shard writes, and settling on an
     * empty dropdown forced a close/reopen. Retry while the scan failed
     * ([result] null — e.g. dumb mode threw) or found nothing, up to the
     * budget; stop the moment anything is offerable.
     */
    fun needsRetry(result: Result?, attempt: Int): Boolean =
        attempt < MAX_SCAN_ATTEMPTS && (result == null || result.candidates.isEmpty())

    /**
     * Merge declared and discovered candidates. [indexRecords] are xref
     * declaration records; only static methods whose simple name is `main`
     * qualify (spec 2.1.2b). De-duplication happens AFTER normalization, so a
     * method the manifest spells `A::main` and the index spells `A.main` is one
     * entry, and the declared standing survives.
     */
    fun merge(
        manifest: CajetaManifest.BuildSettings,
        indexRecords: List<Json.Obj>,
        indexAvailable: Boolean,
    ): Result = merge(listOf(manifest), indexRecords, indexAvailable)

    /**
     * Everything a project's manifests DECLARE, in order, de-duplicated. Pure
     * and index-free — a manifest project must offer its entry method the
     * instant the dialog opens, whatever the index is doing (2026-07-30).
     */
    fun declaredCandidates(manifests: List<CajetaManifest.BuildSettings>): List<Candidate> {
        val out = LinkedHashMap<String, Candidate>()
        // Normalize here rather than trusting the caller: BuildSettings is
        // constructible directly, and spec 1.4.5 asks every consumer to route
        // through the one normalizer. Idempotent, so re-normalizing is free.
        fun declare(raw: String) {
            val fqn = CajetaManifest.normalizeEntryMethod(raw)
            if (fqn.isNotBlank()) out.getOrPut(fqn) { Candidate(fqn, declared = true) }
        }
        for (m in manifests) {
            m.entryMethod?.let(::declare)
            m.binaries.values.forEach(::declare)
        }
        return out.values.toList()
    }

    /** Merge across every manifest the project holds, then discovery. */
    fun merge(
        manifests: List<CajetaManifest.BuildSettings>,
        indexRecords: List<Json.Obj>,
        indexAvailable: Boolean,
    ): Result {
        val out = LinkedHashMap<String, Candidate>()
        declaredCandidates(manifests).forEach { out[it.fqn] = it }   // declared first (2.1.4)

        // Then discovered. getOrPut keeps a declared entry declared.
        for (rec in indexRecords) {
            val fqn = (rec.opt("fqn") as? Json.Str)?.value ?: continue
            if (!isStaticMain(rec, fqn)) continue
            val norm = CajetaManifest.normalizeEntryMethod(fqn)
            out.getOrPut(norm) { Candidate(norm, declared = false) }
        }

        return Result(out.values.toList(), indexAvailable)
    }

    private fun isStaticMain(rec: Json.Obj, fqn: String): Boolean {
        if ((rec.opt("kind") as? Json.Str)?.value != "method") return false
        if (fqn.substringAfterLast('.') != "main") return false
        val mods = (rec.opt("modifiers") as? Json.Arr)?.items ?: return false
        return mods.any { (it as? Json.Str)?.value == "static" }
    }

    /**
     * Every manifest in the project: its own, plus sub-project manifests a
     * bounded scan finds (a repo opened at its root still has entry points in
     * `samples/…`, `tools/…`). File reads only — no index, no read action.
     */
    fun manifestsFor(project: Project): List<CajetaManifest.BuildSettings> {
        val base = project.basePath ?: return emptyList()
        return ManifestScan.findManifests(File(base)).mapNotNull { f ->
            runCatching { CajetaManifest.parseBuildSettings(f.readText()) }.getOrNull()
        }
    }

    /** Declared candidates alone — the fast, index-free first paint (3.2.7). */
    fun declaredForProject(project: Project): List<Candidate> =
        declaredCandidates(manifestsFor(project))

    /**
     * The project's OWN manifest only — ONE small file read, cheap enough to
     * run on the EDT inside [SettingsEditor.resetEditorFrom]. That matters:
     * everything published from a background thread lands via `invokeLater`,
     * which is deferred while a modal dialog is open, so the Run/Debug
     * Configurations dialog would show an empty dropdown no matter how fast
     * the background scan was (3.2.8).
     */
    fun declaredFromRootManifest(project: Project): List<Candidate> =
        declaredCandidates(listOf(CajetaManifest.buildSettings(project)))

    /**
     * Gather the inputs for [project] and merge. Touches the index, so call it
     * off the EDT (3.2.5).
     */
    fun forProject(project: Project): Result {
        val manifests = manifestsFor(project)
        // Only UNAVAILABLE means we cannot look. STALE still has data, and a
        // stale candidate is a fine offer — the developer edits the field, and
        // an out-of-date list is far better than pretending there are none.
        val available = CajetaXrefFreshness.getInstance(project).state !=
            CajetaXrefFreshness.State.UNAVAILABLE
        // FileBasedIndex needs a READ ACTION, not merely a background thread —
        // off-EDT alone trips "Read access is allowed from inside read-action
        // only". Both are required: the read action for correctness, the pooled
        // thread so the settings dialog never blocks on index access.
        val records = if (!available) emptyList() else
            runCatching {
                ReadAction.compute<List<Json.Obj>, RuntimeException> {
                    XrefQuery.fqnsForSimpleName(project, "main")
                        .flatMap { XrefQuery.declarationsOf(project, it) }
                }
            }.getOrDefault(emptyList())
        return merge(manifests, records, available)
    }
}
