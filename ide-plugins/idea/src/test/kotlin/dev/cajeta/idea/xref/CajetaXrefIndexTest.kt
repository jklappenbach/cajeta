package dev.cajeta.idea.xref

import com.intellij.testFramework.fixtures.BasePlatformTestCase
import dev.cajeta.idea.lint.XrefStreamParser

/**
 * ide-symbol-index Unit 6 (plan 6.1.1 - 6.1.6): the index is CACHE, not
 * authority. Xref records — from a whole-root export or a per-edit lint stream
 * — land as per-source-file NDJSON shards under `.cajeta/xref/`, one shard per
 * source file, in the SAME record shape as the lint stream (version line
 * first), so one reader serves both feeds. A FileBasedIndex over the shards
 * gives per-file incremental reindexing (6.1.4) and restart persistence
 * (6.1.5) as platform contracts rather than hand-rolled machinery.
 */
class CajetaXrefIndexTest : BasePlatformTestCase() {

    private fun line(rel: String, record: String) =
        """{"kind":"xref","rel":"$rel","record":$record}"""

    private val version = line("version", """{"major": 1, "minor": 0}""")

    private fun shard(vararg lines: String): String =
        (listOf(version) + lines).joinToString("\n")

    private fun addShard(name: String, text: String) =
        myFixture.addFileToProject("${CajetaXrefShards.DIR}/$name.cjxref", text)

    // ---- 6.1.1 — FQN → declaration; simple name → FQN(s) -----------------------

    fun testFqnAndSimpleNameLookup() {
        addShard("demo_Target", shard(
            line("declarations",
                """{"fqn": "demo.Target", "kind": "class", "file": "demo/Target.cajeta", "line": 2, "col": 13}"""),
            line("declarations",
                """{"fqn": "demo.Target.answer", "kind": "method", "owner": "demo.Target", "overloadKey": "demo.Target::answer(pointer)", "file": "demo/Target.cajeta", "line": 7, "col": 19}"""),
        ))

        val decls = XrefQuery.declarationsOf(project, "demo.Target")
        assertEquals(1, decls.size)
        assertEquals("demo/Target.cajeta", decls[0].at("file").asString())
        assertEquals(2, decls[0].at("line").asInt())

        assertContainsElements(
            XrefQuery.fqnsForSimpleName(project, "Target"), "demo.Target")
        assertContainsElements(
            XrefQuery.fqnsForSimpleName(project, "answer"), "demo.Target.answer")
    }

    // ---- 6.1.2 — reverse edges inverted on ingest -------------------------------

    fun testSubtypesAndCallSitesAreInvertedOnIngest() {
        addShard("demo_A", shard(
            line("inheritance",
                """{"child": "demo.A", "parent": "demo.Base", "kind": "extends", "file": "demo/A.cajeta", "line": 2, "col": 13}"""),
            line("calls",
                """{"callee": "demo.Svc::run(pointer)", "caller": "demo.A::go(pointer)", "file": "demo/A.cajeta", "line": 5, "col": 8}"""),
        ))
        addShard("demo_B", shard(
            line("inheritance",
                """{"child": "demo.B", "parent": "demo.Base", "kind": "extends", "file": "demo/B.cajeta", "line": 2, "col": 13}"""),
            line("calls",
                """{"callee": "demo.Svc::run(pointer)", "caller": "demo.B::go(pointer)", "file": "demo/B.cajeta", "line": 9, "col": 8}"""),
        ))

        val subs = XrefQuery.subtypesOf(project, "demo.Base")
            .map { it.at("child").asString() }.toSet()
        assertEquals(setOf("demo.A", "demo.B"), subs)

        val callers = XrefQuery.callersOf(project, "demo.Svc::run(pointer)")
            .map { it.at("caller").asString() }.toSet()
        assertEquals(setOf("demo.A::go(pointer)", "demo.B::go(pointer)"), callers)
    }

    // ---- 6.1.3 — overload keys survive ingest ------------------------------------

    fun testCallersOfOneOverloadExcludeTheOther() {
        addShard("demo_C", shard(
            line("calls",
                """{"callee": "demo.F::f(pointer,int32)", "caller": "demo.C::a(pointer)", "file": "demo/C.cajeta", "line": 3, "col": 8}"""),
            line("calls",
                """{"callee": "demo.F::f(pointer,cajeta.lang.String)", "caller": "demo.C::b(pointer)", "file": "demo/C.cajeta", "line": 4, "col": 8}"""),
        ))

        val intCallers = XrefQuery.callersOf(project, "demo.F::f(pointer,int32)")
        assertEquals(listOf("demo.C::a(pointer)"),
            intCallers.map { it.at("caller").asString() })
    }

    // ---- 6.1.4 — one file's edit, one shard's records ----------------------------

    fun testRewritingOneShardReplacesOnlyItsRecords() {
        addShard("demo_A", shard(
            line("declarations",
                """{"fqn": "demo.A", "kind": "class", "file": "demo/A.cajeta", "line": 2, "col": 13}"""),
        ))
        addShard("demo_B", shard(
            line("declarations",
                """{"fqn": "demo.B", "kind": "class", "file": "demo/B.cajeta", "line": 2, "col": 13}"""),
        ))
        assertEquals(1, XrefQuery.declarationsOf(project, "demo.A").size)

        // The edit renames A → A2; B's shard is untouched.
        val shardA = myFixture.findFileInTempDir("${CajetaXrefShards.DIR}/demo_A.cjxref")!!
        com.intellij.openapi.command.WriteCommandAction.runWriteCommandAction(project) {
            com.intellij.openapi.vfs.VfsUtil.saveText(shardA, shard(
                line("declarations",
                    """{"fqn": "demo.A2", "kind": "class", "file": "demo/A.cajeta", "line": 2, "col": 13}"""),
            ))
        }

        assertEmpty(XrefQuery.declarationsOf(project, "demo.A"))
        assertEquals(1, XrefQuery.declarationsOf(project, "demo.A2").size)
        assertEquals(1, XrefQuery.declarationsOf(project, "demo.B").size)
    }

    // ---- 6.1.5 / 6.1.6 — versioning ----------------------------------------------

    fun testUnsupportedShardMajorContributesNothing() {
        // Wholesale refusal reaches all the way into the index (§2.0.6).
        addShard("demo_V2", listOf(
            line("version", """{"major": 2, "minor": 0}"""),
            line("declarations",
                """{"fqn": "demo.V2Only", "kind": "class", "file": "demo/V.cajeta", "line": 1, "col": 0}"""),
        ).joinToString("\n"))

        assertEmpty(XrefQuery.declarationsOf(project, "demo.V2Only"))
    }

    fun testIndexVersionDerivesFromSchemaMajorAndGrammar() {
        val v = CajetaXrefIndex.versionFor(XrefStreamParser.SUPPORTED_MAJOR, "atn-1")
        // A schema major bump invalidates; so does any grammar change.
        assertTrue(v != CajetaXrefIndex.versionFor(
            XrefStreamParser.SUPPORTED_MAJOR + 1, "atn-1"))
        assertTrue(v != CajetaXrefIndex.versionFor(
            XrefStreamParser.SUPPORTED_MAJOR, "atn-2"))
        // Deterministic — restart must not spuriously reindex (6.1.5).
        assertEquals(v, CajetaXrefIndex.versionFor(
            XrefStreamParser.SUPPORTED_MAJOR, "atn-1"))
    }

    // ---- shard writer / whole-root splitter (6.2.4) -------------------------------

    fun testShardTextRoundTripsThroughTheStreamReader() {
        val records = XrefStreamParser.demux(shard(
            line("declarations",
                """{"fqn": "demo.T", "kind": "class", "file": "demo/T.cajeta", "line": 1, "col": 0}"""),
            line("references",
                """{"target": "demo.U", "kind": "type", "file": "demo/T.cajeta", "line": 3, "col": 4}"""),
        )).records

        val text = CajetaXrefShards.shardText(records)
        val back = XrefStreamParser.demux(text)
        assertTrue(back.supported)
        assertEquals(records.map { it.rel }, back.records.map { it.rel })
        assertEquals(records.map { it.record.toCompactString() },
            back.records.map { it.record.toCompactString() })
    }

    fun testWholeRootDocumentSplitsIntoPerFileShards() {
        val doc = """
            {
              "version": {"major": 1, "minor": 0},
              "declarations": [
                {"fqn": "demo.A", "kind": "class", "file": "demo/A.cajeta", "line": 1, "col": 0},
                {"fqn": "demo.B", "kind": "class", "file": "demo/B.cajeta", "line": 1, "col": 0}
              ],
              "inheritance": [
                {"child": "demo.B", "parent": "demo.A", "kind": "extends", "file": "demo/B.cajeta", "line": 1, "col": 0}
              ],
              "references": [],
              "overrides": [],
              "calls": []
            }
        """.trimIndent()

        val byFile = CajetaXrefShards.splitDocument(doc)!!
        assertEquals(setOf("demo/A.cajeta", "demo/B.cajeta"), byFile.keys)
        assertEquals(listOf("declarations"), byFile["demo/A.cajeta"]!!.map { it.rel })
        assertEquals(listOf("declarations", "inheritance"),
            byFile["demo/B.cajeta"]!!.map { it.rel })

        // An unknown major refuses the whole document, not part of it.
        assertNull(CajetaXrefShards.splitDocument(
            doc.replace("\"major\": 1", "\"major\": 99")))
    }

    fun testDistinctSourcePathsNeverCollideOnShardName() {
        // demo/A.cajeta vs demo_A.cajeta would collide under naive '_'
        // sanitizing; the hash suffix keeps them apart.
        val a = CajetaXrefShards.shardName("demo/A.cajeta")
        val b = CajetaXrefShards.shardName("demo_A.cajeta")
        assertTrue(a != b)
        assertTrue(a.endsWith(".cjxref"))
    }
}
