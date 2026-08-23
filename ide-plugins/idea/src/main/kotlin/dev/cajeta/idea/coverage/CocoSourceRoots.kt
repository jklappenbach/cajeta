package dev.cajeta.idea.coverage

import com.intellij.openapi.project.Project
import com.intellij.openapi.roots.ProjectRootManager
import dev.cajeta.idea.buildtool.CajetaManifest
import java.io.File

/**
 * The roots coco's relative site paths are resolved against.
 *
 * ## Why this is not just the IDE's module model
 *
 * coco records each site's `file` relative to the root it was pointed at
 * (`tour/coco/Shipping.cajeta`), and the platform's annotator matches
 * `ProjectData`'s keys against real absolute paths. Something has to bridge
 * the two, and `ProjectRootManager.contentSourceRoots` cannot be that thing on
 * its own: a Cajeta project opened as a plain directory gets an `.iml` with a
 * `<content>` element and NO `<sourceFolder>`, so the list is **empty**.
 *
 * The failure that produced this file is the reason it exists. The run loaded —
 * the log said `coco: loaded coverage from …/coco.merged.profile` — and not one
 * gutter appeared, because every site path resolved to nothing and an
 * unresolvable path annotates no file rather than reporting anything. There was
 * no error to find at any layer.
 *
 * The manifest already states which root coco measured. Asking it is
 * authoritative and independent of how the project happens to have been opened,
 * so it goes FIRST; the module model follows for anything the manifest misses.
 *
 * Both callers — [CocoRunLoader] (freshness, analysis) and
 * [CajetaCoverageRunner] (the `ProjectData` the gutters are drawn from) — use
 * this one implementation. They had a copy each, and only the runner's mattered
 * for gutters, so fixing the visible one would have fixed nothing.
 */
object CocoSourceRoots {

    fun of(project: Project?): List<File> {
        if (project == null || project.isDisposed) return emptyList()
        return (listOfNotNull(fromManifest(project)) + fromModuleModel(project)).distinct()
    }

    /** `<manifest dir>/<the root coco measured>`, when it exists on disk. */
    private fun fromManifest(project: Project): File? {
        val manifest = CajetaManifest.path(project) ?: return null
        val base = File(manifest).parentFile ?: return null
        val declared = File(CocoProject.of(project).srcDir)
        val root = if (declared.isAbsolute) declared else File(base, declared.path)
        return root.takeIf { it.isDirectory }
    }

    private fun fromModuleModel(project: Project): List<File> =
        ProjectRootManager.getInstance(project).contentSourceRoots
            .mapNotNull { it.canonicalPath }
            .map(::File)
}
