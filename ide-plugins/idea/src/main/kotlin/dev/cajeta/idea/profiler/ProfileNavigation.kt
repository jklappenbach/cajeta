package dev.cajeta.idea.profiler

import com.intellij.openapi.fileEditor.OpenFileDescriptor
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.LocalFileSystem
import dev.cajeta.idea.coverage.CocoSourceRoots
import java.io.File

/** A frame's location, resolved to something the IDE can open. */
data class ResolvedLocation(
    val file: File,
    /** As the trace recorded it: 1-based, or 0 for "no line". */
    val line: Int,
    /** False when the trace gave no line — the file is right, the line is not. */
    val exact: Boolean,
)

/**
 * cajeta-profiler Unit 11 — from a frame to the code (spec §8.2, §8.4).
 *
 * The trace records a location as the compiler saw it: a path relative to a
 * source root (`tour/Tour.cajeta`), a fully-qualified function name, and a
 * line. The IDE needs a real file.
 *
 * ## Why an unresolved location returns null instead of guessing
 *
 * Nothing in this stack treats an unresolvable path as an error — it simply
 * navigates nowhere. That is how coco shipped a coverage run which loaded
 * cleanly, reported 36.0%, and drew not one gutter: every site path resolved to
 * nothing, silently (`CocoSourceRoots`' own comment records it). Guessing by
 * basename would replace that silence with something worse, landing the reader
 * in a same-named file from another package and giving them no reason to doubt
 * it. Opening nothing is recoverable; opening the wrong thing is not.
 *
 * Roots come from [CocoSourceRoots], which already answers this question for
 * coverage and answers it correctly for the case that matters: a Cajeta project
 * opened as a plain directory has an `.iml` with no `<sourceFolder>`, so the
 * platform's `contentSourceRoots` is EMPTY and the manifest has to be asked
 * first. Re-deriving that here would mean re-learning it here.
 */
object ProfileNavigation {

    /**
     * Resolve against candidate roots, in order. Pure — no project, no VFS — so
     * the rule that decides whether anything opens at all is testable without
     * an IDE.
     */
    fun resolve(location: ProfileSourceLocation, roots: List<File>): ResolvedLocation? {
        if (location.fileName.isEmpty()) return null

        val asGiven = File(location.fileName)
        if (asGiven.isAbsolute) {
            return if (asGiven.isFile) located(asGiven, location) else null
        }
        for (root in roots) {
            val candidate = File(root, location.fileName)
            // isFile, not exists: a directory that happens to share the path is
            // not the source file, and opening one produces an editor showing
            // nothing.
            if (candidate.isFile) return located(candidate, location)
        }
        return null
    }

    private fun located(file: File, location: ProfileSourceLocation) =
        ResolvedLocation(file, location.line, exact = location.line > 0)

    /**
     * Trace lines count from 1; [OpenFileDescriptor] counts from 0. An
     * off-by-one puts every frame one line above where it ran — close enough to
     * look right, and wrong on every frame in the file.
     *
     * Line 0 means the trace had no line (a lambda, in the tour fixture). It
     * maps to the top of the file rather than to -1, and [ResolvedLocation.exact]
     * is what says the line was not known.
     */
    fun editorLine(traceLine: Int): Int = (traceLine - 1).coerceAtLeast(0)

    /** Open the frame's location. Returns false when it could not be resolved. */
    fun open(project: Project, location: ProfileSourceLocation): Boolean {
        val roots = CocoSourceRoots.of(project)
        val resolved = resolve(location, roots) ?: return false
        val vf = LocalFileSystem.getInstance().findFileByIoFile(resolved.file) ?: return false
        OpenFileDescriptor(project, vf, editorLine(resolved.line), 0).navigate(true)
        return true
    }

    /** Open the frame a flame-graph node stands for. */
    fun open(project: Project, node: FlameNode): Boolean {
        val loc = node.sourceLocation ?: return false
        return open(project, loc)
    }
}
