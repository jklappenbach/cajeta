package dev.cajeta.idea.debugger

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotSame
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * resident-debug-server 5.2.2 — the pure keep-alive policy: reuse the live
 * server for the configured binary, respawn when it died or the binary path
 * changed, kill on release. Processes are faked so the policy is the only
 * thing under test.
 */
class ResidentDapCoreTest {

    private class FakeProc(var alive: Boolean = true)

    private fun core(spawned: MutableList<FakeProc>, killed: MutableList<FakeProc>) =
        ResidentDapCore<FakeProc>(
            spawn = { FakeProc().also { spawned.add(it) } },
            isAlive = { it.alive },
            kill = { it.alive = false; killed.add(it) },
        )

    @Test
    fun reusesLiveServerForSameBinary() {
        val spawned = mutableListOf<FakeProc>()
        val core = core(spawned, mutableListOf())
        val a = core.acquire("/bin/cajeta")
        val b = core.acquire("/bin/cajeta")
        assertSame(a.process, b.process)
        assertFalse(a.reused)
        assertTrue(b.reused)
        assertEquals(1, spawned.size)
    }

    @Test
    fun respawnsWhenServerDied() {
        val spawned = mutableListOf<FakeProc>()
        val core = core(spawned, mutableListOf())
        val a = core.acquire("/bin/cajeta")
        a.process.alive = false
        val b = core.acquire("/bin/cajeta")
        assertNotSame(a.process, b.process)
        assertFalse(b.reused)
        assertEquals(2, spawned.size)
    }

    @Test
    fun respawnsAndKillsWhenBinaryPathChanged() {
        val spawned = mutableListOf<FakeProc>()
        val killed = mutableListOf<FakeProc>()
        val core = core(spawned, killed)
        val a = core.acquire("/bin/cajeta")
        val b = core.acquire("/opt/other/cajeta")
        assertNotSame(a.process, b.process)
        assertEquals(listOf(a.process), killed)
    }

    @Test
    fun releaseKillsTheServer() {
        val spawned = mutableListOf<FakeProc>()
        val killed = mutableListOf<FakeProc>()
        val core = core(spawned, killed)
        val a = core.acquire("/bin/cajeta")
        core.release()
        assertEquals(listOf(a.process), killed)
        // Next acquire spawns fresh.
        val b = core.acquire("/bin/cajeta")
        assertNotSame(a.process, b.process)
    }
}
