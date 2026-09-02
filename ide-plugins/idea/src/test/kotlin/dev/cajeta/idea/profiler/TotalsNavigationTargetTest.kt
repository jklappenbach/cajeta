package dev.cajeta.idea.profiler

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Frame name -> the FQN the xref index knows the declaration by.
 *
 * The index holds DECLARATIONS, which carry type parameters, while a frame name
 * carries type ARGUMENTS. Dropping them is the whole mapping, and it has to be
 * balanced: a nested argument list would otherwise truncate the name early and
 * send a query that quietly matches nothing.
 */
class TotalsNavigationTargetTest {

    @Test
    fun typeArgumentsAreDropped() {
        assertEquals("cajeta.lang.Optional.get",
            TotalsNavigation.declarationFqn("cajeta.lang.Optional<int32>.get"))
        assertEquals("cajeta.lang.stream.Stream.forEach",
            TotalsNavigation.declarationFqn("cajeta.lang.stream.Stream<tour.DemoClass>.forEach"))
    }

    @Test
    fun aWildcardArgumentIsDroppedToo() {
        // A generic STATIC method on a generic class renders the class with a
        // wildcard, because the call binds the method's parameter.
        assertEquals("cajeta.nucleo.column.Column.of",
            TotalsNavigation.declarationFqn("cajeta.nucleo.column.Column<?>.of"))
    }

    @Test
    fun nestedArgumentListsDoNotTruncateTheName() {
        assertEquals("cajeta.collection.HashMap.put",
            TotalsNavigation.declarationFqn(
                "cajeta.collection.HashMap<int32,cajeta.collection.ArrayList<String>>.put"))
    }

    @Test
    fun aPlainNameIsUnchanged() {
        assertEquals("tour.Tour.main", TotalsNavigation.declarationFqn("tour.Tour.main"))
    }

    @Test
    fun aLambdaHasNoDeclarationToFind() {
        // Stripping <lambda> as if it were a type argument would leave
        // "tour.Tour." and send a query that can only ever miss.
        assertNull(TotalsNavigation.declarationFqn("tour.Tour.<lambda>"))
    }

    @Test
    fun emptyAndDegenerateNamesYieldNothing() {
        assertNull(TotalsNavigation.declarationFqn(""))
        assertNull(TotalsNavigation.declarationFqn("<int32>"))
    }

    /** A constructor is `Type.Type`; the enclosing type is the second chance. */
    @Test
    fun aConstructorFallsBackToItsClass() {
        val chain = TotalsNavigation.lookupChain("cajeta.lang.Optional.Optional")
        assertEquals(listOf("cajeta.lang.Optional.Optional", "cajeta.lang.Optional"), chain)
    }

    /**
     * And ONLY a constructor. An ordinary method that is simply missing from
     * the index must not silently open its class — that would look like a hit
     * and land the reader somewhere they did not ask for.
     */
    @Test
    fun anOrdinaryMethodDoesNotFallBackToItsClass() {
        val chain = TotalsNavigation.lookupChain("cajeta.lang.Optional.get")
        assertEquals(listOf("cajeta.lang.Optional.get"), chain)
        assertTrue("must not offer the class as a target", chain.none { it == "cajeta.lang.Optional" })
    }
}
