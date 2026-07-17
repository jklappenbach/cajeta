package dev.cajeta.idea.xref

import com.intellij.openapi.project.Project
import com.intellij.openapi.roots.AdditionalLibraryRootsProvider
import com.intellij.openapi.roots.SyntheticLibrary
import com.intellij.openapi.vfs.VirtualFile

/**
 * Exposes the mounted stdlib / dependency source (from [CajetaMountService])
 * to the project model as read-only library SOURCE roots (ide-symbol-index
 * Unit 8, §8.2.2/§8.3). This is what makes those files indexed and navigable:
 * once they are a library source root, `FilenameIndex` finds them and
 * [CajetaXrefReference] resolves a stdlib/dependency declaration record to the
 * real file, so Ctrl-click into `String` / `ArrayList` / `Tensor` / a `.cja`
 * dependency lands on its source.
 *
 * Cheap by construction: it only reads the already-mounted roots the
 * background startup activity stored; the extraction never runs here.
 */
class CajetaXrefLibraryRootsProvider : AdditionalLibraryRootsProvider() {

    override fun getAdditionalProjectLibraries(project: Project): Collection<SyntheticLibrary> {
        val roots = CajetaMountService.getInstance(project).getRoots()
        if (roots.isEmpty()) return emptyList()
        return listOf(SyntheticLibrary.newImmutableLibrary(roots))
    }

    override fun getRootsToWatch(project: Project): Collection<VirtualFile> =
        CajetaMountService.getInstance(project).getRoots()
}
