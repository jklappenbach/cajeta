package dev.cajeta.idea.lint

import java.nio.file.Path

/**
 * lint-own-archive-classpath Unit 5 — the warm daemon's START-TIME context, and
 * how to tell that it has moved.
 *
 * Source root and classpath are `--lint-server` startup flags, not per-request
 * fields ([LintServerRequest]). That is fine while the context is immutable, and
 * the project's own archive is not: `cajeta build` rewrites it at the SAME path,
 * so a daemon spawned before the build keeps answering from a classpath entry
 * whose bytes no longer exist. The path-keyed reasoning everywhere else in the
 * plugin cannot see this — the argv is character-for-character identical across
 * the rebuild.
 *
 * So the context is identified by content, not by path: each classpath entry's
 * path plus the mtime and size of the file at it. That is one `stat` per entry
 * per lint, against a lint that spawns or round-trips a compiler — not
 * measurable. A hash would be exact and costs a full read of every archive on
 * every keystroke, which this is not worth.
 *
 * Pure: the filesystem reading is injected, so the identity's behaviour is
 * testable without building an archive to rebuild.
 */
object LintServerContext {

    /**
     * `cajeta --lint-server` for one project context. The classpath is passed at
     * SPAWN because the server has nowhere else to take it — which is exactly
     * why [identity] exists.
     *
     * An empty classpath emits no flag rather than a bare `--classpath=`, which
     * the compiler would read as one empty entry.
     */
    fun argv(compilerPath: String, sourceRoot: String, classpath: List<Path>): List<String> {
        val argv = mutableListOf(
            compilerPath, "--lint-server",
            "--source-root", sourceRoot,
            "--diag-format=json",
        )
        if (classpath.isNotEmpty())
            argv += "--classpath=" + classpath.joinToString(",") { it.toString() }
        return argv
    }

    /**
     * The identity of a spawned context: what a live server would have to be
     * restarted for.
     *
     * Order is significant — the compiler resolves in classpath order, so two
     * orders are two contexts. A missing file reads as its own stable value
     * rather than as "cannot tell": an archive appearing and an archive vanishing
     * are both real changes and both must be judged.
     */
    fun identity(classpath: List<Path>, stampOf: (Path) -> String = ::stampOf): String =
        classpath.joinToString("\n") { "$it|${stampOf(it)}" }

    /** mtime and size of the file at [p]; `0|0` when there is none. Both, because
     *  a rebuild inside one filesystem timestamp tick is ordinary on a fast
     *  machine and usually changes the length. */
    private fun stampOf(p: Path): String {
        val f = p.toFile()
        return "${f.lastModified()}|${f.length()}"
    }
}
