package dev.cajeta.idea.profiler

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * cajeta-profiler 11.2.e — arming a run through the IDE (spec §9.1, §9.4, §9.5).
 *
 * §9.5 asks that profiling be requestable without hand-editing environment
 * variables. The variables stay the mechanism — there is deliberately no second
 * one, since §9.2 promises no rebuild for the sampling tier — so this turns a
 * checkbox into exactly the environment the runtime already reads.
 */
class CajetaProfileLocationTest {

    private val trace = java.io.File("/tmp/x/run-1.pftrace")

    @Test
    fun armingSetsWhatTheRuntimeReads() {
        val env = CajetaProfileLocation.arm(emptyMap(), trace)
        assertEquals("1", env[CajetaProfileLocation.ENABLE])
        assertEquals(trace.absolutePath, env[CajetaProfileLocation.OUT])
        assertTrue(CajetaProfileLocation.isArmed(env))
    }

    @Test
    fun anExplicitOutputPathIsLeftAlone() {
        // The developer set it for a reason. Redirecting it silently would make
        // the configuration behave differently from the command line it was
        // copied from.
        val env = CajetaProfileLocation.arm(
            mapOf(CajetaProfileLocation.OUT to "/somewhere/mine.pftrace"), trace)
        assertEquals("/somewhere/mine.pftrace", env[CajetaProfileLocation.OUT])
        assertEquals("1", env[CajetaProfileLocation.ENABLE])
    }

    @Test
    fun theRestOfTheEnvironmentSurvivesArming() {
        val env = CajetaProfileLocation.arm(mapOf("PATH" to "/bin", "MY" to "v"), trace)
        assertEquals("/bin", env["PATH"])
        assertEquals("v", env["MY"])
    }

    @Test
    fun disarmingActuallyUnsetsIt() {
        // Unchecking the box must not leave CAJETA_PROFILER=1 behind, which
        // would profile every subsequent run for reasons nobody could see.
        val armed = CajetaProfileLocation.arm(mapOf("PATH" to "/bin"), trace, gpuRing = 4096)
        val off = CajetaProfileLocation.disarm(armed)
        assertFalse(CajetaProfileLocation.isArmed(off))
        assertFalse(off.containsKey(CajetaProfileLocation.OUT))
        assertFalse(off.containsKey(CajetaProfileLocation.GPU_RING))
        assertEquals("/bin", off["PATH"])
    }

    @Test
    fun theGpuRingIsOnlySetWhenAskedFor() {
        assertFalse(CajetaProfileLocation.arm(emptyMap(), trace)
            .containsKey(CajetaProfileLocation.GPU_RING))
        assertEquals("8192", CajetaProfileLocation.arm(emptyMap(), trace, gpuRing = 8192)
            [CajetaProfileLocation.GPU_RING])
        assertFalse(CajetaProfileLocation.arm(emptyMap(), trace, gpuRing = 0)
            .containsKey(CajetaProfileLocation.GPU_RING))
    }

    @Test
    fun anEmptyEnableValueIsNotArmed() {
        assertFalse(CajetaProfileLocation.isArmed(mapOf(CajetaProfileLocation.ENABLE to "")))
    }

    // --- naming ---------------------------------------------------------------

    @Test
    fun aConfigurationNameBecomesAFilesystemSafeStem() {
        assertEquals("my-run", CajetaProfileLocation.sanitize("my run"))
        assertEquals("a-b", CajetaProfileLocation.sanitize("a/b"))
        assertEquals("Tour_main", CajetaProfileLocation.sanitize("Tour_main"))
    }

    @Test
    fun aNameThatSanitizesToNothingStillProducesAFile() {
        // A run configuration may legitimately be named "/" or "...".
        assertEquals("profile", CajetaProfileLocation.sanitize("/"))
        assertEquals("profile", CajetaProfileLocation.sanitize(""))
    }
}
