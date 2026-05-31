package dev.cajeta.idea.debugger

import java.io.File

/**
 * Builds and starts the `cajeta dap` child process. Centralizes two
 * platform concerns so production code and tests share them:
 *  - the command is always `<binary> dap` (stdio DAP server);
 *  - on Windows the mingw-linked binary needs libstdc++/libgcc on PATH, so an
 *    optional DLL directory is prepended to the child's PATH (otherwise the
 *    process fails to load with 0xC000007B).
 *
 * The pure command/PATH builders are unit-tested; [start] is exercised by the
 * session integration test against the real binary.
 */
class CajetaDapLauncher(
    private val binaryPath: String,
    private val dllDir: String? = null,
) {
    fun command(): List<String> = listOf(binaryPath, "dap")

    /** PATH for the child: dllDir prepended to [basePath] when set. */
    fun childPath(basePath: String?): String = when {
        dllDir.isNullOrBlank() -> basePath ?: ""
        basePath.isNullOrBlank() -> dllDir
        else -> dllDir + File.pathSeparator + basePath
    }

    fun start(workingDir: File? = null): Process {
        val pb = ProcessBuilder(command())
        pb.redirectErrorStream(false)
        if (workingDir != null) pb.directory(workingDir)
        if (!dllDir.isNullOrBlank()) {
            val env = pb.environment()
            val key = env.keys.firstOrNull { it.equals("PATH", ignoreCase = true) } ?: "PATH"
            env[key] = childPath(env[key])
        }
        return pb.start()
    }

    companion object {
        /** The mingw64 bin dir on Windows, if present; else null. */
        fun defaultDllDir(): String? {
            System.getenv("CAJETA_DLL_DIR")?.let { return it }
            val def = File("C:\\msys64\\mingw64\\bin")
            return if (def.isDirectory) def.absolutePath else null
        }

        /**
         * Best-effort discovery of a built `cajeta` binary for tests/dev:
         * CAJETA_DAP_BIN env, else the in-tree build/src output relative to the
         * plugin module. Returns null if nothing executable is found.
         */
        fun locateBinary(): File? {
            System.getenv("CAJETA_DAP_BIN")?.let {
                val f = File(it)
                if (f.canExecute()) return f
            }
            val isWindows = System.getProperty("os.name").lowercase().contains("win")
            val exe = if (isWindows) "cajeta.exe" else "cajeta"
            val moduleDir = File(System.getProperty("user.dir"))
            return listOf(
                File(moduleDir, "../../build/src/$exe"),
                File(moduleDir, "../../../build/src/$exe"),
            ).map { it.absoluteFile.normalize() }.firstOrNull { it.canExecute() }
        }
    }
}
