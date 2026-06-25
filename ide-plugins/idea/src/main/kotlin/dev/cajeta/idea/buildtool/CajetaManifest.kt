package dev.cajeta.idea.buildtool

import com.intellij.openapi.project.Project
import java.io.File

/** Locates the project's `cajeta.json` manifest (spec §2.2: the tool window is
 *  only available when one exists). */
object CajetaManifest {
    fun path(project: Project): String? {
        val base = project.basePath ?: return null
        val f = File(base, "cajeta.json")
        return if (f.isFile) f.path else null
    }

    fun exists(project: Project): Boolean = path(project) != null
}
