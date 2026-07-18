package dev.cajeta.idea.xref

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.LocalFileSystem
import dev.cajeta.idea.debugger.Json
import dev.cajeta.idea.lint.XrefRecord
import dev.cajeta.idea.lint.XrefStreamParser
import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.Paths

/**
 * Per-source-file xref shards (ide-symbol-index Unit 6). Both feeds — the
 * whole-root export document and the per-edit lint stream — land here as one
 * NDJSON shard per SOURCE file under `<project>/.cajeta/xref/`, in the exact
 * record shape the lint stream carries (version line first). One reader
 * ([XrefStreamParser]) serves both feeds and the index; per-file shards are
 * what make the FileBasedIndex incremental per source file (plan 6.1.4).
 */
object CajetaXrefShards {

    const val DIR = ".cajeta/xref"

    private val log = Logger.getInstance(CajetaXrefShards::class.java)

    /**
     * Shard file name for a root-relative source path. Sanitized for the
     * filesystem plus a hash suffix — `demo/A.cajeta` and `demo_A.cajeta`
     * must never share a shard.
     */
    fun shardName(sourceRelPath: String): String {
        val safe = sourceRelPath.replace(Regex("[^A-Za-z0-9._-]"), "_")
        val hash = Integer.toHexString(sourceRelPath.hashCode())
        return "$safe-$hash.cjxref"
    }

    /** Shard content: the stream shape, version record first. */
    fun shardText(records: List<XrefRecord>): String = buildString {
        append("""{"kind":"xref","rel":"version","record":{"major": """)
        append(XrefStreamParser.SUPPORTED_MAJOR)
        append(""", "minor": 0}}""").append('\n')
        for (r in records) {
            append("""{"kind":"xref","rel":"""").append(r.rel)
            append("""","record":""").append(r.record.toCompactString())
            append('}').append('\n')
        }
    }

    /**
     * Split a whole-root export document into per-source-file record groups.
     * Returns null when the document declares a schema major this plugin does
     * not speak — refused WHOLESALE, never partially read (§2.0.6).
     */
    fun splitDocument(documentText: String): Map<String, List<XrefRecord>>? {
        val root = try {
            Json.parse(documentText) as? Json.Obj
        } catch (e: Exception) {
            null
        } ?: return null
        val major = (root.opt("version")?.opt("major") as? Json.Num)?.value?.toInt()
        if (major != XrefStreamParser.SUPPORTED_MAJOR) return null

        val byFile = LinkedHashMap<String, MutableList<XrefRecord>>()
        for (rel in listOf("declarations", "inheritance", "references",
                           "overrides", "calls")) {
            val arr = root.opt(rel) as? Json.Arr ?: continue
            for (item in arr.items) {
                val record = item as? Json.Obj ?: continue
                val file = (record.opt("file") as? Json.Str)?.value ?: continue
                byFile.getOrPut(file) { mutableListOf() } += XrefRecord(rel, record)
            }
        }
        return byFile
    }

    /** Write one source file's shard. Call off the EDT; refreshes the VFS. */
    fun writeShard(project: Project, sourceRelPath: String,
                   records: List<XrefRecord>) {
        val base = project.basePath ?: return
        val dir = Paths.get(base, *DIR.split('/').toTypedArray())
        try {
            Files.createDirectories(dir)
            gitIgnore(dir.parent)
            val shard = dir.resolve(shardName(sourceRelPath))
            Files.write(shard, shardText(records)
                .toByteArray(StandardCharsets.UTF_8))
            refresh(shard)
        } catch (e: Exception) {
            log.warn("xref shard write failed for $sourceRelPath: ${e.message}")
        }
    }

    /** Ingest a whole-root export document: one shard per source file. */
    fun ingestDocument(project: Project, documentText: String): Boolean {
        val byFile = splitDocument(documentText) ?: return false
        for ((file, records) in byFile) writeShard(project, file, records)
        return true
    }

    /**
     * Ingest a per-edit lint stream. The stream is file-scoped by contract
     * (Unit 3), but group by the records' own `file` field rather than trust.
     * A version-only stream (broken buffer) writes nothing: KEEP the previous
     * shard (spec 2.0.5).
     */
    fun ingestStream(project: Project, records: List<XrefRecord>) {
        if (records.isEmpty()) return
        records.groupBy { (it.record.opt("file") as? Json.Str)?.value }
            .forEach { (file, recs) ->
                if (file != null) writeShard(project, file, recs)
            }
    }

    private fun gitIgnore(cajetaDir: Path) {
        val gi = cajetaDir.resolve(".gitignore")
        if (!Files.exists(gi)) {
            runCatching { Files.write(gi, "xref/\n".toByteArray()) }
        }
    }

    private fun refresh(path: Path) {
        val refresh = Runnable {
            LocalFileSystem.getInstance()
                .refreshAndFindFileByNioFile(path)
        }
        val app = ApplicationManager.getApplication()
        if (app.isDispatchThread) refresh.run()
        else app.invokeLater(refresh)
    }
}
