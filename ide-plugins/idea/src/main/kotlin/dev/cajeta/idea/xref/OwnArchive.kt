package dev.cajeta.idea.xref

import java.nio.file.Path

/**
 * lint-own-archive-classpath Unit 2 — where the project's OWN `.cja` comes from,
 * and how it joins the lint classpath.
 *
 * Lint resolves a file against one source root plus a classpath. For a file in a
 * test source root the project's own types live in neither the root nor the
 * dependency cache, so without the project's own archive they cannot resolve at
 * all — measured on `cajeta-http`: 13 unresolved types, 0 once the archive is
 * added. `run-tests.sh` already compiles such a suite exactly this way.
 *
 * The path comes from `cajeta artifact-path`, which prints the DECLARED path
 * without building. Never glob `build/archive`: it accumulates old versions
 * (0.1.3 and 0.1.4 sitting beside 0.2.0 in cajeta-http), and a glob picks an
 * arbitrary one — which still resolves, so the mistake hides.
 *
 * Pure by design: the subprocess and the filesystem are injected, so this needs
 * neither a compiler nor the IntelliJ platform. Production wiring lives in
 * [CajetaSourceMountGlue].
 */
object OwnArchive {

    /**
     * Flavors tried in order. `artifact-path` defaults to `release`, but a
     * developer's working tree is as often only built `debug` — and the two
     * frequently name the SAME file anyway (both flavors print
     * `dev.cajeta.http-0.2.0.cja` for cajeta-http). Trying release then debug
     * finds a built archive in either case; the first whose file exists wins.
     */
    val DEFAULT_FLAVORS = listOf("release", "debug")

    /**
     * The project's own archive, or null when there is none to use.
     *
     * Null covers three different facts, all of which mean "lint without it"
     * rather than "fail": the manifest declares no artifact (non-zero exit), the
     * declared archive has never been built (named but absent — `artifact-path`
     * reports where it *would* go), and the invocation itself failed. Callers
     * report the absence per spec §4.2; none of them treat it as an error.
     */
    fun resolve(
        flavors: List<String> = DEFAULT_FLAVORS,
        runArtifactPath: (flavor: String) -> Pair<Int, String>,
        exists: (Path) -> Boolean,
    ): Path? {
        for (flavor in flavors) {
            val (exitCode, stdout) = try {
                runArtifactPath(flavor)
            } catch (e: Exception) {
                return null   // a compiler we cannot run will not answer for another flavor
            }
            if (exitCode != 0) continue
            val declared = parse(stdout) ?: continue
            if (exists(declared)) return declared
        }
        return null
    }

    /**
     * The lint classpath: dependencies first, exactly as given, with the own
     * archive appended. Dependency entries are never reordered or dropped —
     * a dependency type that stops resolving would clobber its Ctrl-click
     * targets on the next per-edit lint.
     *
     * `plusElement`, never `+`: [Path] implements `Iterable<Path>`, so
     * `List<Path> + Path` binds to `plus(Iterable)` and splatters the archive
     * into its name segments — `/p/build/archive/lib.cja` becomes the four
     * entries `p`, `build`, `archive`, `lib.cja`, none of which is a real file.
     * Silent: the classpath is still a `List<Path>` of the right type.
     */
    fun classpath(dependencies: List<Path>, own: Path?): List<Path> =
        when {
            own == null -> dependencies
            dependencies.contains(own) -> dependencies
            else -> dependencies.plusElement(own)
        }

    /**
     * Per-project memo so `artifact-path` does not spawn on every keystroke
     * (2.3.2). Pure: clock and mtime probe are injected, so a test can count
     * resolutions instead of trusting that caching happens.
     *
     * A found archive stays valid while its mtime is unchanged — a rebuild
     * moves it and forces a re-resolve. An ABSENT result is held only for
     * [ttlMs], so a project built mid-session is picked up without an IDE
     * restart (spec §4.4); without that expiry the negative would be permanent
     * and "build it" would never take effect.
     */
    class Cache(
        private val ttlMs: Long = 5_000L,
        private val now: () -> Long = System::currentTimeMillis,
    ) {
        private data class Entry(val path: Path?, val stamp: Long?, val checkedAt: Long)

        private val entries = java.util.concurrent.ConcurrentHashMap<String, Entry>()

        fun get(key: String, mtimeOf: (Path) -> Long?, resolve: () -> Path?): Path? {
            val hit = entries[key]
            if (hit != null && isFresh(hit, mtimeOf)) return hit.path
            val resolved = resolve()
            entries[key] = Entry(resolved, resolved?.let(mtimeOf), now())
            return resolved
        }

        private fun isFresh(e: Entry, mtimeOf: (Path) -> Long?): Boolean =
            if (e.path == null) now() - e.checkedAt < ttlMs else mtimeOf(e.path) == e.stamp
    }

    /** The first line naming an archive. `artifact-path` prints one path, but
     *  tolerate surrounding noise rather than depending on it being alone. */
    private fun parse(stdout: String): Path? =
        stdout.lineSequence()
            .map { it.trim() }
            .firstOrNull { it.endsWith(".cja") }
            ?.let { Path.of(it) }
}
