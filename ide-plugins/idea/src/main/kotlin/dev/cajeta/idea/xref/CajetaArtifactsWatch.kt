package dev.cajeta.idea.xref

/**
 * Which VFS events mean "a resolved dependency archive landed for this
 * project" (ide-symbol-index §8.3 first-open fix): a `.cja` under the
 * project's `.cajeta/cache/artifacts/` — the content-hash directory the
 * resolver populates on the first successful build, which is also exactly
 * what [CajetaSourceMountGlue.dependencyArchives] mounts from. Pure.
 */
object CajetaArtifactsWatch {

    fun isArtifactArchivePath(path: String, basePath: String?): Boolean {
        if (basePath == null) return false
        if (!path.endsWith(".cja")) return false
        return path.startsWith(basePath.trimEnd('/') + "/.cajeta/cache/artifacts/")
    }
}
