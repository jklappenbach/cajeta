package dev.cajeta.idea.lint

import java.io.File
import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.TimeUnit

/**
 * lint-own-archive-classpath Unit 1 (1.2.1 / 1.2.2) — a real project on disk and
 * a thin harness that lints it.
 *
 * The project is scaffolded from the toolchain's own `library` archetype rather
 * than from a hand-written manifest, so the fixture cannot drift from the real
 * manifest schema. Nothing depends on a `cajeta-http` checkout (plan 1.3.3).
 *
 * The compiler comes from `CAJETA_TEST_COMPILER`; without it every test that
 * uses this is skipped rather than failing, so the suite still runs on a machine
 * with no compiler built.
 */
class LintFixture private constructor(
    val root: Path,
    val mainRoot: String,
    val testRoot: String,
    val testFile: String,
    val bogusFile: String,
) {

    /** The declared artifact path, from `cajeta artifact-path` — never a glob.
     *  A build directory accumulates old versions (measured: 0.1.3 and 0.1.4
     *  sitting beside 0.2.0), and a glob picks an arbitrary one. */
    fun artifact(): Path {
        val out = run(listOf(compiler(), "artifact-path"), root)
        val line = out.lineSequence().firstOrNull { it.trim().endsWith(".cja") }
            ?: error("artifact-path printed no archive path:\n$out")
        return Path.of(line.trim())
    }

    /** Lint one file and return the type names reported as unresolved, in
     *  encounter order without duplicates. Matches on the error id itself, not
     *  on prose (plan 1.3.2). */
    fun lintUnresolved(file: String, sourceRoot: String, classpath: List<Path>): List<String> {
        val argv = mutableListOf(compiler(), "--lint", file, "--source-root", sourceRoot)
        if (classpath.isNotEmpty()) argv += "--classpath=" + classpath.joinToString(",")
        return unresolvedFrom(argv)
    }

    /** Raw compiler output for an argv built by the PLUGIN, for tests that read
     *  something other than diagnostics out of it — the `--emit-xref` stream. */
    fun outputOf(argv: List<String>): String = run(argv, root)

    /** Run an argv built by the PLUGIN and report the same thing — so a test can
     *  assert against what the plugin would really invoke, not a hand-rolled
     *  command line that happens to agree with it. */
    fun unresolvedFrom(argv: List<String>): List<String> = unresolvedIn(run(argv, root))

    /** Absolute form of a fixture-relative path, for argv builders that do not
     *  run with the project as their working directory. */
    fun abs(rel: String): String = root.resolve(rel).toString()

    /** The whole-root xref export over [sourceRoot] — the document the shard is
     *  ingested from, and therefore what Ctrl-click resolves against. Returns
     *  the raw JSON. */
    fun exportXref(sourceRoot: String, classpath: List<Path>): String {
        val out = Files.createTempFile("cajeta-xref-", ".json")
        val argv = mutableListOf(compiler(), "--lint", sourceRoot,
                                 "--emit-xref=$out", "--diag-format=json")
        if (classpath.isNotEmpty()) argv += "--classpath=" + classpath.joinToString(",")
        run(argv, root)
        return try {
            Files.readString(out)
        } catch (e: Exception) {
            ""
        } finally {
            runCatching { Files.deleteIfExists(out) }
        }
    }

    /** The configured compiler, for tests that build an argv themselves. */
    fun compilerPath(): String = compiler()

    fun delete() {
        root.toFile().deleteRecursively()
    }

    companion object {
        /** The unresolved-type names in a block of compiler output, wherever it
         *  came from. The warm `--lint-server` answers with the SAME NDJSON on
         *  its stdout that a one-shot writes to stderr (lint-server-spec §5), so
         *  both paths are read by this one function — which is also what makes
         *  comparing them (plan 5.3.1) mean anything. */
        fun unresolvedIn(output: String): List<String> =
            output.lineSequence()
                .filter { it.contains(UNRESOLVED_TYPE) }
                .mapNotNull { QUOTED.find(it)?.groupValues?.get(1) }
                .distinct()
                .toList()

        private const val UNRESOLVED_TYPE = "CAJETA_ERROR_UNRESOLVED_TYPE"
        private val QUOTED = Regex("unresolved type '([^']+)'")
        private const val PKG = "com/example/library"

        fun compilerAvailable(): Boolean {
            val p = System.getenv("CAJETA_TEST_COMPILER") ?: return false
            return p.isNotBlank() && File(p).canExecute()
        }

        private fun compiler(): String =
            System.getenv("CAJETA_TEST_COMPILER") ?: error("CAJETA_TEST_COMPILER is not set")

        /** Library in `src/main/cajeta`, its tests in a SEPARATE `test/src` root
         *  — the layout that exposes the defect. */
        fun twoRoot(): LintFixture {
            val dir = scaffold("twoRoot")
            write(
                dir, "test/src/com/example/test/UseGreeter.cajeta",
                """
                package com.example.test;

                import com.example.library.Greeter;

                public final class UseGreeter {
                    public static int32 run() {
                        Greeter g = heap Greeter();
                        return 0;
                    }
                }
                """.trimIndent(),
            )
            write(
                dir, "test/src/com/example/test/Bogus.cajeta",
                """
                package com.example.test;

                import com.example.library.Greeter;

                public final class Bogus {
                    public static int32 run() {
                        Greeter g = heap Greeter();
                        NoSuchTypeAtAll z = null;
                        return 0;
                    }
                }
                """.trimIndent(),
            )
            build(dir)
            return LintFixture(
                root = dir,
                mainRoot = "src/main/cajeta",
                testRoot = "test/src",
                testFile = "test/src/com/example/test/UseGreeter.cajeta",
                bogusFile = "test/src/com/example/test/Bogus.cajeta",
            )
        }

        /** Everything in ONE root — the control. A sibling resolves with no
         *  archive at all, and must keep resolving when one is added. */
        fun singleRoot(): LintFixture {
            val dir = scaffold("singleRoot")
            write(
                dir, "src/main/cajeta/$PKG/Sibling.cajeta",
                """
                package com.example.library;

                public final class Sibling {
                    public static int32 run() {
                        Greeter g = heap Greeter();
                        return 0;
                    }
                }
                """.trimIndent(),
            )
            build(dir)
            return LintFixture(
                root = dir,
                mainRoot = "src/main/cajeta",
                testRoot = "src/main/cajeta",
                testFile = "src/main/cajeta/$PKG/Sibling.cajeta",
                bogusFile = "src/main/cajeta/$PKG/Sibling.cajeta",
            )
        }

        private fun scaffold(name: String): Path {
            val tmp = Files.createTempDirectory("cajeta-lint-fixture-")
            val out = run(listOf(compiler(), "init", "library", name), tmp)
            val dir = tmp.resolve(name)
            check(Files.isDirectory(dir)) { "`cajeta init library` produced no project:\n$out" }
            return dir
        }

        private fun build(dir: Path) {
            val out = run(listOf(compiler(), "build"), dir)
            check(out.contains(".cja")) { "fixture build produced no archive:\n$out" }
        }

        private fun write(dir: Path, rel: String, body: String) {
            val f = dir.resolve(rel)
            Files.createDirectories(f.parent)
            Files.writeString(f, body + "\n")
        }

        private fun run(argv: List<String>, cwd: Path): String {
            val p = ProcessBuilder(argv)
                .directory(cwd.toFile())
                .redirectErrorStream(true)
                .start()
            val out = p.inputStream.bufferedReader().readText()
            if (!p.waitFor(10, TimeUnit.MINUTES)) {
                p.destroyForcibly()
                error("timed out: ${argv.joinToString(" ")}")
            }
            return out
        }
    }
}
