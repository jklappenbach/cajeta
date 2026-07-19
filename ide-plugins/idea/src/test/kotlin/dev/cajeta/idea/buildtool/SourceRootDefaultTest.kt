package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * run-config-ergonomics 2.1 — source-root defaulting (spec §3).
 *
 * Precedence: manifest `source-root` resolved against the project base, then
 * `<base>/src/main/cajeta` when it exists, then `<base>`. Pure functions over a
 * real temp tree, so no Project is needed.
 */
class SourceRootDefaultTest {

    @get:Rule
    val tmp = TemporaryFolder()

    // 2.1.1 / spec 3.2.1
    @Test
    fun manifestSourceRootWinsAndResolvesAgainstBase() {
        val base = tmp.newFolder("proj")
        assertEquals(
            File(base, "src/main/cajeta").path,
            CajetaRoots.defaultSourceRoot(base.path, "src/main/cajeta"))
    }

    // 2.1.1 — an absolute manifest value is used as-is, not re-rooted.
    @Test
    fun absoluteManifestSourceRootIsUsedVerbatim() {
        val base = tmp.newFolder("proj2")
        val abs = tmp.newFolder("elsewhere")
        assertEquals(abs.path, CajetaRoots.defaultSourceRoot(base.path, abs.path))
    }

    // 2.1.2 / spec 3.2.2
    @Test
    fun conventionalLayoutIsUsedWhenManifestIsSilent() {
        val base = tmp.newFolder("proj3")
        File(base, "src/main/cajeta").mkdirs()
        assertEquals(
            File(base, "src/main/cajeta").path,
            CajetaRoots.defaultSourceRoot(base.path, null))
    }

    // 2.1.3 / spec 3.2.3 — a flat project defaults to the base, never empty.
    @Test
    fun flatProjectDefaultsToProjectBase() {
        val base = tmp.newFolder("proj4")
        assertEquals(base.path, CajetaRoots.defaultSourceRoot(base.path, null))
    }

    // 2.1.4 / spec 3.2.4 — a deliberate value is never replaced by a default.
    @Test
    fun persistedValueIsNeverOverwritten() {
        val base = tmp.newFolder("proj5")
        File(base, "src/main/cajeta").mkdirs()
        assertEquals("/my/own/root",
            CajetaRoots.sourceRootFor("/my/own/root", base.path, "src/main/cajeta"))
    }

    // 2.1.4 — blank persisted value falls through to the default.
    @Test
    fun blankPersistedValueFallsThroughToTheDefault() {
        val base = tmp.newFolder("proj6")
        assertEquals(base.path, CajetaRoots.sourceRootFor("", base.path, null))
        assertEquals(base.path, CajetaRoots.sourceRootFor("   ", base.path, null))
    }

    // 2.1.5 / spec 3.1.3 — the run config and the xref rebuild must describe the
    // same tree. Asserted against the SHARED function both call, so the two can
    // not drift; a duplicated literal here would prove nothing.
    @Test
    fun defaultAgreesWithTheXrefRebuildDerivation() {
        val withSrc = tmp.newFolder("proj7")
        File(withSrc, "src/main/cajeta").mkdirs()
        assertEquals(
            CajetaRoots.conventionalSourceRoot(withSrc.path),
            CajetaRoots.defaultSourceRoot(withSrc.path, null))

        val flat = tmp.newFolder("proj8")
        assertEquals(
            CajetaRoots.conventionalSourceRoot(flat.path),
            CajetaRoots.defaultSourceRoot(flat.path, null))
    }
}
