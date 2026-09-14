package dev.cajeta.idea.lint

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.file.Path

/**
 * lint-own-archive-classpath Unit 3 — the own archive reaches the one-shot lint
 * argv, and the source root does NOT change.
 *
 * The defect was never the source root: `sourceRootOf` derives it from the
 * file's package and is correct. What was missing is the project's own `.cja`
 * on the classpath, which is how `run-tests.sh` already compiles the same suite.
 */
class LintClasspathTest {

    private val deps = listOf(
        Path.of("/p/.cajeta/cache/artifacts/aa.cja"),
        Path.of("/p/.cajeta/cache/artifacts/bb.cja"),
    )
    private val own = Path.of("/p/build/archive/lib-0.2.0.cja")

    /** 3.1.3 — one `--classpath=`, comma-joined, dependencies first. */
    @Test
    fun argvCarriesDependenciesThenTheOwnArchiveInOneFlag() {
        val argv = CajetacRunner.lintArgv(
            "/c", "/tmp/T.cajeta", "/p/test/src", "/p/test/src/a/T.cajeta",
            emitXref = false, classpath = deps.plusElement(own),
        )
        val cp = argv.filter { it.startsWith("--classpath=") }
        assertEquals("exactly one --classpath flag", 1, cp.size)
        assertEquals(
            "--classpath=/p/.cajeta/cache/artifacts/aa.cja," +
                "/p/.cajeta/cache/artifacts/bb.cja,/p/build/archive/lib-0.2.0.cja",
            cp.single(),
        )
    }

    /** 3.1.5 — with no own archive the argv is exactly what it was before this
     *  work: dependencies only, nothing appended, nothing reordered. */
    @Test
    fun withNoOwnArchiveTheArgvIsUnchanged() {
        val before = CajetacRunner.lintArgv(
            "/c", "/tmp/T.cajeta", "/p/test/src", "/p/test/src/a/T.cajeta",
            emitXref = false, classpath = deps,
        )
        val viaClasspathHelper = CajetacRunner.lintArgv(
            "/c", "/tmp/T.cajeta", "/p/test/src", "/p/test/src/a/T.cajeta",
            emitXref = false,
            classpath = dev.cajeta.idea.xref.OwnArchive.classpath(deps, null),
        )
        assertEquals(before, viaClasspathHelper)
    }

    /** 3.1.2 — the source root stays the package-derived one. This is the
     *  cajeta-http shape: a file in `test/src` whose package is
     *  `dev.cajeta.http.test` strips back to `test/src`, NOT to the project
     *  root and NOT to the main source root. Unit 3 must not change it. */
    @Test
    fun theSourceRootIsStillDerivedFromThePackage() {
        val root = CajetacRunner.sourceRootOf(
            "/p/test/src/dev/cajeta/http/test/ServerTests.cajeta",
            "package dev.cajeta.http.test;\npublic final class ServerTests {}",
        )
        assertEquals("/p/test/src", root)
    }

    /** 3.1.2 — and it is what lands in the argv. */
    @Test
    fun thatSameRootIsWhatTheArgvCarries() {
        val file = "/p/test/src/dev/cajeta/http/test/ServerTests.cajeta"
        val root = CajetacRunner.sourceRootOf(file, "package dev.cajeta.http.test;\nclass ServerTests {}")
        val argv = CajetacRunner.lintArgv("/c", "/tmp/staged.cajeta", root, file,
                                          emitXref = false, classpath = deps.plusElement(own))
        val i = argv.indexOf("--source-root")
        assertTrue("--source-root must be present", i >= 0)
        assertEquals("/p/test/src", argv[i + 1])
    }
}
