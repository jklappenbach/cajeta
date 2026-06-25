package dev.cajeta.idea.buildtool

import com.intellij.openapi.diagnostic.Logger
import java.io.IOException
import java.nio.charset.StandardCharsets
import java.util.concurrent.TimeUnit

/**
 * Spawns the `cajeta` build tool for discovery and (later) task runs (spec
 * §15.2), modeled on `lint/CajetacRunner`. Blocking calls — invoke OFF the EDT
 * (§15.1): the tool window runs discovery under a background task. The
 * build-tool path is validated up-front via [BuildToolPathValidator] so a bad
 * path is surfaced, not a silent empty tree (§14.2.1, §15.3).
 */
object CajetaBuildRunner {

    private val log = Logger.getInstance(CajetaBuildRunner::class.java)

    sealed interface DiscoverResult {
        data class Ok(val model: TaskModel) : DiscoverResult
        data class Failed(val reason: String) : DiscoverResult
    }

    /** Run `cajeta tasks --json --manifest=<path>` and parse it. Blocking. */
    fun discover(
        buildToolPath: String,
        manifestPath: String,
        timeoutMs: Long = 10_000,
    ): DiscoverResult {
        BuildToolPathValidator.problem(buildToolPath)?.let {
            return DiscoverResult.Failed("build tool path: $it")
        }
        val argv = listOf(buildToolPath) + CajetaCommandLine.discoveryArgv(manifestPath)
        return try {
            val result = spawn(argv, timeoutMs)
            when {
                result.timedOut ->
                    DiscoverResult.Failed("discovery timed out after ${timeoutMs}ms")
                result.exitCode != 0 ->
                    DiscoverResult.Failed(
                        "`cajeta tasks --json` exited ${result.exitCode}: ${result.stderr.trim()}",
                    )
                else -> when (val r = TaskDiscoveryParser.parse(result.stdout)) {
                    is TaskDiscoveryParser.Result.Success -> DiscoverResult.Ok(r.model)
                    is TaskDiscoveryParser.Result.Failure ->
                        DiscoverResult.Failed("could not parse discovery output: ${r.reason}")
                }
            }
        } catch (e: IOException) {
            DiscoverResult.Failed("failed to run ${argv.joinToString(" ")}: ${e.message}")
        }
    }

    data class SpawnResult(
        val exitCode: Int,
        val stdout: String,
        val stderr: String,
        val timedOut: Boolean,
    )

    /**
     * Spawn [argv], draining stdout and stderr on separate threads (so neither
     * pipe buffer can deadlock the child), bounded by [timeoutMs]. The child is
     * force-killed on timeout. Throws [IOException] only if the process can't be
     * started.
     */
    fun spawn(argv: List<String>, timeoutMs: Long): SpawnResult {
        val process = ProcessBuilder(argv).redirectErrorStream(false).start()
        process.outputStream.close()

        val outBuf = StringBuilder()
        val errBuf = StringBuilder()
        val outPump = pump(process.inputStream, outBuf)
        val errPump = pump(process.errorStream, errBuf)

        val finished = process.waitFor(timeoutMs, TimeUnit.MILLISECONDS)
        if (!finished) {
            process.destroyForcibly()
            log.warn("cajeta spawn timed out: ${argv.joinToString(" ")}")
        }
        outPump.join(1_000)
        errPump.join(1_000)
        return SpawnResult(
            exitCode = if (finished) process.exitValue() else -1,
            stdout = outBuf.toString(),
            stderr = errBuf.toString(),
            timedOut = !finished,
        )
    }

    private fun pump(stream: java.io.InputStream, into: StringBuilder): Thread =
        Thread {
            try {
                stream.bufferedReader(StandardCharsets.UTF_8).use { reader ->
                    val chunk = CharArray(4096)
                    while (true) {
                        val n = reader.read(chunk)
                        if (n < 0) break
                        synchronized(into) { into.append(chunk, 0, n) }
                    }
                }
            } catch (_: IOException) {
                // stream closed on process death — nothing to recover.
            }
        }.apply { isDaemon = true; start() }
}
