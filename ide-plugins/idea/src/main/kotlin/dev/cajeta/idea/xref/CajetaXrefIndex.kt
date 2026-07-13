package dev.cajeta.idea.xref

import com.intellij.util.indexing.DataIndexer
import com.intellij.util.indexing.FileBasedIndex
import com.intellij.util.indexing.FileBasedIndexExtension
import com.intellij.util.indexing.FileContent
import com.intellij.util.indexing.ID
import com.intellij.util.io.DataExternalizer
import com.intellij.util.io.EnumeratorStringDescriptor
import com.intellij.util.io.KeyDescriptor
import dev.cajeta.idea.debugger.Json
import dev.cajeta.idea.lint.XrefStreamParser
import dev.cajeta.idea.parser.antlr.CajetaParser
import java.io.DataInput
import java.io.DataOutput

/**
 * The xref index (ide-symbol-index Unit 6, spec §5): CACHE, not authority. One
 * FileBasedIndex over the `.cjxref` shards, string keys prefixed by relation,
 * values = newline-joined compact-JSON records. The two reverse relations the
 * compiler does not hold — parent → subtypes, callee → call sites — are built
 * here by inversion at ingest (§5.0.2).
 *
 * Keys:
 *   fqn:<fqn>            declaration records for the FQN
 *   name:<simpleName>    declared FQNs whose last segment is <simpleName>
 *   sub:<parentFqn>      inheritance records whose parent is <parentFqn>
 *   call:<calleeKey>     call records to that overloadKey (never conflated
 *                        across overloads — the key IS the overload, §5.0.3)
 *   use:<relFile>        reference + call records sited in that source file
 *   uses:<targetFqn>     reference records targeting the FQN
 *   ovr:<overloadKey>    override records whose `overrides` is the key
 *   ovrof:<overloadKey>  override records whose `method` is the key
 */
class CajetaXrefIndex : FileBasedIndexExtension<String, String>() {

    override fun getName(): ID<String, String> = NAME

    override fun getVersion(): Int =
        versionFor(XrefStreamParser.SUPPORTED_MAJOR, CajetaParser._serializedATN)

    override fun dependsOnFileContent(): Boolean = true

    override fun getInputFilter(): FileBasedIndex.InputFilter =
        FileBasedIndex.InputFilter { it.extension == "cjxref" }

    override fun getKeyDescriptor(): KeyDescriptor<String> =
        EnumeratorStringDescriptor.INSTANCE

    override fun getValueExternalizer(): DataExternalizer<String> = STRING_UTF8

    override fun getIndexer(): DataIndexer<String, String, FileContent> =
        DataIndexer { input ->
            val stream = XrefStreamParser.demux(input.contentAsText.toString())
            // Unsupported major → nothing enters the index: wholesale refusal
            // reaches all the way down (§2.0.6).
            if (!stream.supported) return@DataIndexer emptyMap()

            val map = HashMap<String, MutableList<String>>()
            fun put(key: String, value: String) =
                map.getOrPut(key) { mutableListOf() }.add(value)

            fun str(r: Json.Obj, field: String): String? =
                (r.opt(field) as? Json.Str)?.value

            for (r in stream.records) {
                val rec = r.record
                when (r.rel) {
                    "declarations" -> {
                        val fqn = str(rec, "fqn") ?: continue
                        put("fqn:$fqn", rec.toCompactString())
                        put("name:${fqn.substringAfterLast('.')}", fqn)
                    }
                    "inheritance" -> {
                        val parent = str(rec, "parent") ?: continue
                        put("sub:$parent", rec.toCompactString())
                    }
                    "references" -> {
                        val target = str(rec, "target") ?: continue
                        put("uses:$target", rec.toCompactString())
                        str(rec, "file")?.let {
                            put("use:$it", rec.toCompactString())
                        }
                    }
                    "calls" -> {
                        val callee = str(rec, "callee") ?: continue
                        put("call:$callee", rec.toCompactString())
                        str(rec, "file")?.let {
                            put("use:$it", rec.toCompactString())
                        }
                    }
                    "overrides" -> {
                        str(rec, "overrides")?.let {
                            put("ovr:$it", rec.toCompactString())
                        }
                        str(rec, "method")?.let {
                            put("ovrof:$it", rec.toCompactString())
                        }
                    }
                    // Unknown rel: carried by the parser, ignored at USE time.
                }
            }
            map.mapValues { it.value.joinToString("\n") }
        }

    companion object {
        val NAME: ID<String, String> = ID.create("dev.cajeta.xref")

        /**
         * Index version from the schema major AND a grammar fingerprint
         * (spec §5.0.6): either changing must invalidate every shard's
         * mappings. Deterministic, so a plain restart never reindexes.
         */
        fun versionFor(schemaMajor: Int, grammarFingerprint: String): Int =
            (schemaMajor shl 20) xor (grammarFingerprint.hashCode() and 0xFFFFF)

        private val STRING_UTF8 = object : DataExternalizer<String> {
            override fun save(out: DataOutput, value: String) =
                com.intellij.util.io.IOUtil.writeUTF(out, value)
            override fun read(input: DataInput): String =
                com.intellij.util.io.IOUtil.readUTF(input)
        }
    }
}
