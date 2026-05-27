package dev.cajeta.idea.harness

import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.Path

/**
 * A single fixture loaded from disk. `kind` is derived from the
 * subdirectory the fixture lives under ("valid" or "invalid"), which
 * tells the harness whether intermediate parse errors are expected.
 */
data class Fixture(
    val displayName: String,
    val path: Path,
    val kind: Kind,
) {
    enum class Kind { VALID, INVALID }

    fun text(): String = Files.readString(path, StandardCharsets.UTF_8)
}

object FixtureLoader {

    /** Scans valid/ and invalid/ subdirectories for .cajeta files, sorted. */
    fun load(rootDir: Path): List<Fixture> {
        if (!Files.isDirectory(rootDir)) return emptyList()
        val out = mutableListOf<Fixture>()
        for (kind in Fixture.Kind.entries) {
            val kindDir = rootDir.resolve(kind.name.lowercase())
            if (!Files.isDirectory(kindDir)) continue
            Files.walk(kindDir).use { stream ->
                stream
                    .filter { Files.isRegularFile(it) && it.toString().endsWith(".cajeta") }
                    .sorted()
                    .forEach { file ->
                        val relative = kindDir.relativize(file).toString()
                        val display = "${kind.name.lowercase()}/$relative"
                        out += Fixture(display, file, kind)
                    }
            }
        }
        return out
    }
}
