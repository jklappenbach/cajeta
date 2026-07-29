package dev.cajeta.idea.debugger

/**
 * The pure resident-server keep-alive policy (resident-debug-server 5.2.2):
 * reuse the live process for the configured binary; respawn when it died or
 * the binary path changed (killing the old one); kill on release. Process
 * creation is injected so the policy is unit-testable without processes —
 * [CajetaResidentDapService] binds it to real ones.
 */
class ResidentDapCore<P : Any>(
    private val spawn: (binary: String) -> P,
    private val isAlive: (P) -> Boolean,
    private val kill: (P) -> Unit,
) {
    data class Acquired<P>(val process: P, val reused: Boolean)

    private var process: P? = null
    private var binary: String? = null

    @Synchronized
    fun acquire(binaryPath: String): Acquired<P> {
        val live = process
        if (live != null && binary == binaryPath && isAlive(live)) {
            return Acquired(live, reused = true)
        }
        releaseLocked()
        val fresh = spawn(binaryPath)
        process = fresh
        binary = binaryPath
        return Acquired(fresh, reused = false)
    }

    @Synchronized
    fun release() = releaseLocked()

    private fun releaseLocked() {
        process?.let { runCatching { kill(it) } }
        process = null
        binary = null
    }
}
