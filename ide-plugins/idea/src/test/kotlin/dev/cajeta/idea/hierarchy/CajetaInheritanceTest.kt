package dev.cajeta.idea.hierarchy

import dev.cajeta.idea.debugger.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class CajetaInheritanceTest {

    private fun rec(json: String): Json.Obj = Json.parse(json) as Json.Obj

    @Test
    fun anEdgeCarriesBothEndpointsAndItsKind() {
        val e = CajetaInheritance.parse(rec(
            """{"child":"demo.Dog","parent":"demo.Animal","kind":"extends",
                "file":"demo/Dog.cajeta","line":4}"""))!!
        assertEquals("demo.Dog", e.child)
        assertEquals("demo.Animal", e.parent)
        assertFalse(e.isInterface)
        assertEquals(4, e.line)
    }

    @Test
    fun implementsMarksAnInterface() {
        val e = CajetaInheritance.parse(rec(
            """{"child":"demo.Dog","parent":"demo.Walker","kind":"implements"}"""))!!
        assertTrue(e.isInterface)
    }

    @Test
    fun anEdgeMissingAnEndpointIsNotAnEdge() {
        assertNull(CajetaInheritance.parse(rec("""{"child":"demo.Dog"}""")))
        assertNull(CajetaInheritance.parse(rec("""{"parent":"demo.Animal"}""")))
        assertNull(CajetaInheritance.parse(rec("""{"child":"","parent":"x"}""")))
    }

    @Test
    fun parentsListClassesBeforeInterfaces() {
        // 3.1.3: a class implementing several interfaces shows all of them.
        val edges = CajetaInheritance.parseAll(listOf(
            rec("""{"child":"d.C","parent":"d.Zed","kind":"implements"}"""),
            rec("""{"child":"d.C","parent":"d.Base","kind":"extends"}"""),
            rec("""{"child":"d.C","parent":"d.Alpha","kind":"implements"}"""),
        ))
        assertEquals(listOf("d.Base", "d.Alpha", "d.Zed"),
            CajetaInheritance.parentsOf(edges).map { it.parent })
    }

    @Test
    fun childrenAreDedupedAndOrdered() {
        val edges = CajetaInheritance.parseAll(listOf(
            rec("""{"child":"d.Beta","parent":"d.Base","kind":"extends"}"""),
            rec("""{"child":"d.Alpha","parent":"d.Base","kind":"extends"}"""),
            rec("""{"child":"d.Beta","parent":"d.Base","kind":"extends"}"""),
        ))
        assertEquals(listOf("d.Alpha", "d.Beta"),
            CajetaInheritance.childrenOf(edges).map { it.child })
    }

    @Test
    fun theClosureVisitsEachTypeOnce() {
        // A diamond: D -> B, D -> C, B -> A, C -> A. A appears once.
        val up = mapOf(
            "D" to listOf("B", "C"), "B" to listOf("A"), "C" to listOf("A"))
        assertEquals(listOf("B", "C", "A"),
            CajetaInheritance.closure("D") { up[it].orEmpty() })
    }

    @Test
    fun aCycleTerminatesTheWalk() {
        // A broken index can say X extends Y extends X. That must stop the
        // walk, not the IDE.
        val up = mapOf("X" to listOf("Y"), "Y" to listOf("X"))
        assertEquals(listOf("Y"), CajetaInheritance.closure("X") { up[it].orEmpty() })
    }

    @Test
    fun anAbsurdHierarchyIsBounded() {
        val chain = { n: String -> listOf("t" + (n.removePrefix("t").toInt() + 1)) }
        assertEquals(50, CajetaInheritance.closure("t0", limit = 50) { chain(it) }.size)
    }

    @Test
    fun nodeLabelsReadLikeJavas() {
        assertEquals("Dog  (demo.pets)", CajetaInheritance.displayName("demo.pets.Dog"))
        assertEquals("Dog", CajetaInheritance.displayName("Dog"))
    }
}
