package dev.cajeta.idea.coverage

import com.intellij.openapi.components.Service
import com.intellij.openapi.project.Project
import java.io.File
import java.security.MessageDigest
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicReference

/**
 * Per-file freshness of a loaded coverage run (spec §5).
 *
 * Coverage drawn against source that has since changed is worse than none: it is
 * confidently wrong, and nothing downstream can tell. This follows the
 * [dev.cajeta.idea.xref.CajetaXrefFreshness] precedent — a queryable state with
 * an actionable reason, so consumers degrade honestly instead of guessing.
 *
 * ## Per file, not per project
 *
 * The common case is one file edited out of many. Blanking the whole display for
 * that would make the feature useless at exactly the moment it is being used, so
 * a file's neighbours changing never affects it (spec §5.2, plan 5.1.c).
 *
 * ## Content, not the clock
 *
 * A file is fresh iff **its content is what the run measured**. Modification
 * time alone is wrong in both directions and both matter here:
 *
 *  - The IDE saves constantly. If a bumped mtime meant stale, every file would
 *    go stale within seconds of loading a run.
 *  - A file edited between the run finishing and the results being opened was
 *    measured in a state never observed, so it cannot be adopted as measured —
 *    mtime is what detects that, and it is the only thing that can.
 *
 * So the measured content is captured at load, and only for files that still
 * predate the run; anything newer is stale from the start.
 */
@Service(Service.Level.PROJECT)
class CocoFreshness {

    enum class State {
        /** Content matches what the run measured. */
        FRESH,

        /** Changed since the run — markings must not be drawn. */
        STALE,

        /** The run said nothing about this file. */
        UNKNOWN,
    }

    private data class Measured(val digest: String?, val reason: String?)

    /** Cheap re-check: skip re-hashing when neither size nor mtime moved. */
    private data class Probe(val mtime: Long, val length: Long, val digest: String)

    private val measured = ConcurrentHashMap<String, Measured>()
    private val probes = ConcurrentHashMap<String, Probe>()
    private val run = AtomicReference<File?>(null)
    private val runAt = AtomicReference<Long?>(null)

    val origin: File? get() = run.get()
    val runTimestamp: Long? get() = runAt.get()

    /** One line naming the run's origin and age, for §5.4. */
    fun describeRun(): String? {
        val f = run.get() ?: return null
        val at = runAt.get() ?: return null
        return "${f.path} (${java.util.Date(at)})"
    }

    /**
     * Record what the run measured.
     *
     * Reads every covered file, so callers run it off the EDT — the load path
     * already does (2.2.e).
     */
    fun observeRun(profile: File, paths: Collection<String>) {
        val at = profile.lastModified()
        run.set(profile)
        runAt.set(at)
        measured.clear()
        probes.clear()
        for (path in paths) {
            val f = File(path)
            measured[path] = when {
                !f.isFile ->
                    Measured(null, "the file no longer exists")
                // Newer than the run: it changed after being measured, and what
                // was measured is unknowable from here.
                f.lastModified() > at ->
                    Measured(null, "changed after the coverage run")
                else ->
                    Measured(digestOf(f), null)
            }
        }
    }

    fun stateOf(path: String): State {
        val m = measured[path] ?: return State.UNKNOWN
        val expected = m.digest ?: return State.STALE
        val f = File(path)
        if (!f.isFile) return State.STALE
        return if (digestOf(f) == expected) State.FRESH else State.STALE
    }

    fun isStale(path: String): Boolean = stateOf(path) == State.STALE

    /** Why [path] is stale, or null when it is not. */
    fun reasonFor(path: String): String? {
        val m = measured[path] ?: return null
        if (stateOf(path) != State.STALE) return null
        return m.reason ?: "edited since the coverage run"
    }

    fun staleFiles(): List<String> =
        measured.keys.filter { stateOf(it) == State.STALE }.sorted()

    fun clear() {
        measured.clear()
        probes.clear()
        run.set(null)
        runAt.set(null)
    }

    /**
     * SHA-256 of the file, memoized on (mtime, length).
     *
     * The memo matters: this is consulted per gutter query, and re-reading every
     * open file on every repaint would be felt.
     */
    private fun digestOf(f: File): String {
        val mtime = f.lastModified()
        val length = f.length()
        probes[f.path]?.let { if (it.mtime == mtime && it.length == length) return it.digest }
        val digest = try {
            MessageDigest.getInstance("SHA-256").digest(f.readBytes())
                .joinToString("") { "%02x".format(it) }
        } catch (e: java.io.IOException) {
            // Unreadable is not fresh. A digest that cannot be computed must not
            // compare equal to the measured one.
            return "<unreadable:${System.identityHashCode(e)}>"
        }
        probes[f.path] = Probe(mtime, length, digest)
        return digest
    }

    companion object {
        fun getInstance(project: Project): CocoFreshness =
            project.getService(CocoFreshness::class.java)
    }
}
