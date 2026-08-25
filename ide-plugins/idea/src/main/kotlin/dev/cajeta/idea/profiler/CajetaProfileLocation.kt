package dev.cajeta.idea.profiler

import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.openapi.vfs.VirtualFile
import java.io.File

/**
 * cajeta-profiler 11.2.e — where traces live, and how a run is asked to write
 * one (spec §9.1, §9.4, §9.5).
 *
 * §9.5 asks that profiling be requestable through the build tool "without
 * hand-editing environment variables". The variables are still what the runtime
 * reads — there is deliberately no second mechanism (§9.2: no rebuild for the
 * sampling tier) — so this turns a checkbox into exactly the environment the
 * runtime already understands.
 *
 * Kept free of IDE types where it can be, because the part worth testing is the
 * naming and the arming, not the file chooser.
 */
object CajetaProfileLocation {

    const val ENABLE = "CAJETA_PROFILER"
    const val OUT = "CAJETA_PROFILER_OUT"
    const val GPU_RING = "CAJETA_PROFILER_GPU_RING"

    /** What the runtime writes when `CAJETA_PROFILER_OUT` is unset (§9.4). */
    const val DEFAULT_NAME = "cajeta.pftrace"

    /** Traces from IDE runs are collected here rather than in the CWD. */
    const val PROFILE_DIR = "build/cajeta/profiles"

    /**
     * The environment a profiled run needs, layered onto what the configuration
     * already carries.
     *
     * An explicit `CAJETA_PROFILER_OUT` the developer typed is LEFT ALONE. They
     * set it for a reason, and silently redirecting their output to our
     * directory would make a configuration behave differently from the command
     * line that produced it.
     */
    fun arm(
        existing: Map<String, String>,
        traceFile: File,
        gpuRing: Int? = null,
    ): Map<String, String> {
        val out = LinkedHashMap(existing)
        out[ENABLE] = "1"
        out.putIfAbsent(OUT, traceFile.absolutePath)
        if (gpuRing != null && gpuRing > 0) out.putIfAbsent(GPU_RING, gpuRing.toString())
        return out
    }

    /** Strip profiling back out, so unchecking the box actually unchecks it. */
    fun disarm(existing: Map<String, String>): Map<String, String> {
        val out = LinkedHashMap(existing)
        out.remove(ENABLE)
        out.remove(OUT)
        out.remove(GPU_RING)
        return out
    }

    fun isArmed(env: Map<String, String>): Boolean =
        env[ENABLE]?.isNotEmpty() == true

    /**
     * Where a given run's trace goes.
     *
     * Named from the configuration and a caller-supplied stamp, so consecutive
     * runs of the same configuration do not overwrite each other — comparing a
     * run against the one before it is the common reason to profile twice, and
     * a fixed name makes that impossible.
     */
    fun traceFile(project: Project, configurationName: String, stamp: String): File =
        File(profileDirectory(project), "${sanitize(configurationName)}-$stamp.pftrace")

    fun profileDirectory(project: Project): File {
        val base = project.basePath?.let(::File) ?: File(".")
        return File(base, PROFILE_DIR)
    }

    /** Where the Open action starts looking. */
    fun defaultDirectory(project: Project): VirtualFile? {
        val fs = LocalFileSystem.getInstance()
        val profiles = profileDirectory(project)
        if (profiles.isDirectory) fs.findFileByIoFile(profiles)?.let { return it }
        return project.basePath?.let { fs.findFileByPath(it) }
    }

    /**
     * The newest trace under the project, or null.
     *
     * Both locations are searched: our own directory, and the project root,
     * where a run started from a terminal leaves `cajeta.pftrace`. A developer
     * who profiled from the command line and then reached for the IDE should
     * not have to explain where the file is.
     */
    fun mostRecent(project: Project): File? {
        val candidates = ArrayList<File>()
        profileDirectory(project).listFiles { f -> f.extension == "pftrace" }
            ?.let { candidates.addAll(it) }
        project.basePath?.let { File(it, DEFAULT_NAME) }
            ?.takeIf { it.isFile }?.let { candidates.add(it) }
        return candidates.maxByOrNull { it.lastModified() }
    }

    /** Filesystem-safe, and never empty — a run configuration may be named "/". */
    internal fun sanitize(name: String): String {
        val cleaned = name.map { if (it.isLetterOrDigit() || it == '-' || it == '_') it else '-' }
            .joinToString("")
            .trim('-')
        return cleaned.ifEmpty { "profile" }
    }
}
