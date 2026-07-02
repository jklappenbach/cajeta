package dev.cajeta.idea.buildtool

import java.io.File
import java.io.InputStream
import java.nio.charset.StandardCharsets

/**
 * Production [BuildTaskProcess]: spawns `cajeta <task> …` and streams its stdout
 * and stderr to the sink incrementally (separate drain threads so neither pipe
 * buffer can deadlock the child — mirrors `CajetaBuildRunner.spawn`, but pushes
 * chunks live instead of buffering to exit). `cancel` force-kills the child
 * (spec §5.1); `run` then returns `cancelled = true`.
 */
class ProcessBuildTaskProcess(
    private val argv: List<String>,
    private val workDir: String?,
) : BuildTaskProcess {

    @Volatile private var process: Process? = null
    @Volatile private var cancelled = false

    override fun run(sink: OutputSink): ProcessOutcome {
        val pb = ProcessBuilder(argv).redirectErrorStream(false)
        workDir?.let { pb.directory(File(it)) }
        val p = pb.start().also { process = it }
        p.outputStream.close()

        val out = pump(p.inputStream, sink, stdout = true)
        val err = pump(p.errorStream, sink, stdout = false)
        val code = p.waitFor()
        out.join(1_000)
        err.join(1_000)
        return ProcessOutcome(exitCode = code, cancelled = cancelled)
    }

    override fun cancel() {
        cancelled = true
        process?.destroyForcibly()
    }

    private fun pump(stream: InputStream, sink: OutputSink, stdout: Boolean): Thread =
        Thread {
            try {
                stream.bufferedReader(StandardCharsets.UTF_8).use { reader ->
                    val chunk = CharArray(4096)
                    while (true) {
                        val n = reader.read(chunk)
                        if (n < 0) break
                        sink.append(String(chunk, 0, n), stdout)
                    }
                }
            } catch (_: Exception) {
                // stream closed on process death — nothing to recover.
            }
        }.apply { isDaemon = true; start() }
}
