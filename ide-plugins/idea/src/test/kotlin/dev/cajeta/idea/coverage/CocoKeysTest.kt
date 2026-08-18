package dev.cajeta.idea.coverage

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * ide-coverage-plan Unit 6.2.a — the key normalization the two sides must agree
 * on, or the whole dead-code analysis is quietly wrong.
 *
 * coco keys methods as `owner::name/argc`, deliberately **collapsing overloads
 * by arity** (its own words: "keys collapse overloads by arity … so the analysis
 * can miss dead code but can never call a live method dead"). The compiler's
 * xref `overloadKey` is the other shape — `demo.Box::get(T)`, params as written.
 * Reachability joins the two, so it has to convert, and a conversion that is
 * wrong in the unreachable direction proposes deleting live code.
 */
class CocoKeysTest {

    // --- from a coco site ----------------------------------------------------

    @Test
    fun aSiteOwnerAndMethodBecomeAnArityKey() {
        assertEquals("probe.Cond::both/2", CocoKeys.ofSite("probe.Cond", "both(b:int32,a:int32)"))
        assertEquals("probe.Cond::main/0", CocoKeys.ofSite("probe.Cond", "main()"))
        assertEquals("probe.Helper::twice/1", CocoKeys.ofSite("probe.Helper", "twice(n:int32)"))
    }

    // --- from the compiler's overloadKey -------------------------------------

    @Test
    fun anOverloadKeyIsCollapsedToArity() {
        assertEquals("demo.Box::get/1", CocoKeys.ofOverloadKey("demo.Box::get(T)"))
        assertEquals("demo.Box::of/0", CocoKeys.ofOverloadKey("demo.Box::of()"))
        assertEquals("a.b.C::m/3", CocoKeys.ofOverloadKey("a.b.C::m(int32,String,bool)"))
    }

    @Test
    fun genericArgumentsDoNotInflateTheArity() {
        // `Map<K,V>` is ONE parameter. Counting its comma would key the method
        // as /2, match nothing, and report a live method as unreachable — the
        // one error direction coco's design forbids.
        assertEquals("demo.S::put/1", CocoKeys.ofOverloadKey("demo.S::put(Map<K,V>)"))
        assertEquals("demo.S::put/2", CocoKeys.ofOverloadKey("demo.S::put(Map<K,V>,int32)"))
        assertEquals(
            "demo.S::deep/1",
            CocoKeys.ofOverloadKey("demo.S::deep(Map<K,List<Pair<A,B>>>)"),
        )
    }

    @Test
    fun arraysAndWhitespaceDoNotChangeTheArity() {
        assertEquals("demo.S::f/2", CocoKeys.ofOverloadKey("demo.S::f(int32[], String)"))
        assertEquals("demo.S::g/1", CocoKeys.ofSite("demo.S", "g( n : int32 )"))
    }

    @Test
    fun anUnparseableKeyIsNullRatherThanAGuess() {
        // A key that cannot be normalized must not become a key that matches
        // nothing — that reads as "unreachable" and invites a deletion.
        assertNull(CocoKeys.ofOverloadKey("no-separator-here"))
        assertNull(CocoKeys.ofOverloadKey(""))
        assertNull(CocoKeys.ofSite("demo.S", "noParens"))
    }

    @Test
    fun theTwoSpellingsOfTheSameMethodAgree() {
        // The whole point: a site and an xref record for one method must key
        // identically, or coverage and reachability never join.
        assertEquals(
            CocoKeys.ofSite("probe.Cond", "both(b:int32,a:int32)"),
            CocoKeys.ofOverloadKey("probe.Cond::both(int32,int32)"),
        )
    }
}
