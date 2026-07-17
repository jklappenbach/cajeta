package dev.cajeta.idea.xref

import com.intellij.openapi.components.Service
import com.intellij.openapi.components.service
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile
import java.util.concurrent.atomic.AtomicReference

/**
 * Holds the project's mounted read-only source roots (stdlib, and later
 * dependency `.cja` sources) so [CajetaXrefLibraryRootsProvider] can expose
 * them to the project model instantly — the slow extraction runs once in the
 * background ([CajetaStdlibMountStartup]) and stores the result here.
 *
 * Separate from [CajetaMountedSources] (which tracks the same dirs for the
 * DEBUGGER's frame-path lookup): that set is not the project model, so the
 * editor never indexed or navigated into it. This service is what makes the
 * mounted source a real, indexed, navigable library root.
 */
@Service(Service.Level.PROJECT)
class CajetaMountService {
    private val roots = AtomicReference<List<VirtualFile>>(emptyList())

    fun getRoots(): List<VirtualFile> = roots.get().filter { it.isValid }

    fun setRoots(newRoots: List<VirtualFile>) {
        roots.set(newRoots.toList())
    }

    companion object {
        fun getInstance(project: Project): CajetaMountService = project.service()
    }
}
