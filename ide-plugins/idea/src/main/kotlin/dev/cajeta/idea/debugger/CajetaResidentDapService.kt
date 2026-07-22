package dev.cajeta.idea.debugger

import com.intellij.openapi.Disposable
import com.intellij.openapi.components.Service

/**
 * Project-scoped owner of the resident `cajeta dap` process
 * (resident-debug-server §2): one server per project, reused across debug
 * sessions, respawned when dead or when the compiler binary setting moved,
 * killed with the project (Disposable). The stderr pump outlives sessions;
 * each session rebinds [stderrSink] to its own console.
 */
@Service(Service.Level.PROJECT)
class CajetaResidentDapService : Disposable {

    class Server(val process: Process, val client: DapClient)

    /** Rebound by each session so resident stderr lands in ITS console. */
    @Volatile
    var stderrSink: ((String) -> Unit)? = null

    private val core = ResidentDapCore<Server>(
        spawn = { binary ->
            val proc = CajetaDapLauncher(binary, CajetaDapLauncher.defaultDllDir()).start()
            // One pump for the process's lifetime; the sink is per-session.
            StderrPump(proc.errorStream) { line -> stderrSink?.invoke(line) }.start()
            Server(proc, DapClient(DapTransport(proc.inputStream, proc.outputStream)))
        },
        isAlive = { it.process.isAlive },
        kill = { it.process.destroyForcibly() },
    )

    /** The live server for [binary], spawning/respawning as needed. Event
     *  handlers are cleared so the new session's registrations stand alone. */
    fun acquire(binary: String): Server {
        val acquired = core.acquire(binary)
        acquired.process.client.resetEventHandlers()
        return acquired.process
    }

    /** Drop the server (stuck debuggee, identity refusal): the next acquire
     *  spawns fresh. */
    fun releaseForRespawn() = core.release()

    override fun dispose() = core.release()
}
