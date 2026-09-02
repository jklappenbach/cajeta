package dev.cajeta.idea.settings

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files

/**
 * The compiler path had NO validation at all: empty, missing, a directory or
 * not executable all rendered identically to a working path, and Settings said
 * nothing (Julian, 2026-08-31: "No compiler configured does not tell me 'no
 * compiler configured'"). The build-tool row next to it has had a red problem
 * label since it shipped — the asymmetry was backwards, because everything
 * (lint, xref, Ctrl-click, run configs, the debugger) routes through the
 * compiler path and only coco routes through the build tool.
 *
 * The check itself is a pure string+stat verdict, so it is shared rather than
 * copied: a second implementation would drift from the one whose messages
 * people have learned to read.
 */
class ExecutablePathValidatorTest {

    @Test
    fun anEmptyPathIsNamedByItsLabel() {
        val r = ExecutablePathValidator.validate("", "Compiler path")
        assertTrue(r is ExecutablePathValidator.Result.Invalid)
        assertEquals("Compiler path is empty", (r as ExecutablePathValidator.Result.Invalid).reason)
    }

    @Test
    fun aBareCommandNameDefersToPathLookup() {
        val r = ExecutablePathValidator.validate("cajeta", "Compiler path")
        assertTrue(r is ExecutablePathValidator.Result.Ok)
        assertNotNull((r as ExecutablePathValidator.Result.Ok).note)
    }

    @Test
    fun aMissingFileIsNamed() {
        val problem = ExecutablePathValidator.problem("/no/such/cajeta", "Compiler path")
        assertNotNull(problem)
        assertTrue(problem!!.contains("/no/such/cajeta"))
    }

    @Test
    fun aDirectoryIsNotAnExecutable() {
        val dir = Files.createTempDirectory("cajeta-exe-check")
        try {
            val problem = ExecutablePathValidator.problem(dir.toString(), "Compiler path")
            assertNotNull(problem)
            assertTrue(problem!!.contains("directory"))
        } finally {
            Files.deleteIfExists(dir)
        }
    }

    @Test
    fun aNonExecutableFileIsNamed() {
        val f = Files.createTempFile("cajeta-exe-check", ".bin")
        try {
            File(f.toString()).setExecutable(false)
            val problem = ExecutablePathValidator.problem(f.toString(), "Compiler path")
            assertNotNull(problem)
            assertTrue(problem!!.contains("executable"))
        } finally {
            Files.deleteIfExists(f)
        }
    }

    @Test
    fun anExecutableFileHasNoProblem() {
        val f = Files.createTempFile("cajeta-exe-check", ".bin")
        try {
            File(f.toString()).setExecutable(true)
            assertNull(ExecutablePathValidator.problem(f.toString(), "Compiler path"))
        } finally {
            Files.deleteIfExists(f)
        }
    }

    // The build tool delegates to this, so its messages must not change — the
    // point of sharing is one vocabulary, not two that happen to agree today.
    @Test
    fun theBuildToolDelegatesAndKeepsItsOwnLabel() {
        assertEquals(
            "Build-tool path is empty",
            dev.cajeta.idea.buildtool.BuildToolPathValidator.problem(""),
        )
    }
}
