package dev.cajeta.idea.debugger

import com.intellij.testFramework.fixtures.BasePlatformTestCase
import com.intellij.xdebugger.frame.XFullValueEvaluator
import com.intellij.xdebugger.frame.XValueNode
import com.intellij.xdebugger.frame.XValuePlace
import com.intellij.xdebugger.frame.presentation.XValuePresentation
import javax.swing.Icon

/**
 * variable-inspection §4.1.5 — what the Variables row actually shows.
 *
 * [TypeColumnTest] pins the shortening itself; this pins that CajetaValue
 * REACHES it, on BOTH presentation paths. The two paths are separate
 * `setPresentation` overloads (plain leaf vs. facet-decorated), and wiring one
 * while leaving the other on the raw `variable.type` is exactly the failure a
 * pure shortener test cannot see.
 *
 * Needs the platform only for `CajetaSettings.instance` on the facet path.
 */
class CajetaValueTest : BasePlatformTestCase() {

    /** Captures whichever setPresentation overload the value chose. */
    private class RecordingNode : XValueNode {
        var type: String? = null
        var value: String? = null
        var hasChildren: Boolean? = null
        var presentation: XValuePresentation? = null

        override fun setPresentation(icon: Icon?, type: String?, value: String, hasChildren: Boolean) {
            this.type = type
            this.value = value
            this.hasChildren = hasChildren
        }

        override fun setPresentation(icon: Icon?, presentation: XValuePresentation, hasChildren: Boolean) {
            this.presentation = presentation
            this.type = presentation.type
            this.hasChildren = hasChildren
        }

        override fun setFullValueEvaluator(evaluator: XFullValueEvaluator) = Unit
        override fun isObsolete(): Boolean = false
    }

    private fun present(v: DapVariable): RecordingNode {
        val node = RecordingNode()
        CajetaValue(v).computePresentation(node, XValuePlace.TREE)
        return node
    }

    fun testPlainLeafShowsTheSimpleType() {
        val node = present(
            DapVariable(name = "name", value = "\"Ada\"", type = "cajeta.lang.String", variablesReference = 0),
        )
        assertEquals("String", node.type)
        assertEquals("\"Ada\"", node.value)
        assertEquals(false, node.hasChildren)
    }

    fun testFacetDecoratedRowShowsTheSimpleTypeToo() {
        val node = present(
            DapVariable(
                name = "origin",
                value = "{x=3, y=4}",
                type = "tour.geom.Point",
                variablesReference = 7,
                facets = MemoryFacets(alloc = AllocClass.HEAP, ownership = OwnershipRole.OWNER),
            ),
        )
        // The facet path takes the XValuePresentation overload, so the type
        // arrives through getType() rather than as a parameter.
        assertNotNull(node.presentation)
        assertEquals("Point", node.type)
    }

    /**
     * 4.2.5's other half: `hasChildren` follows the SERVER's reference, not a
     * guess from the type name. A non-zero reference is the server saying "I
     * minted a handle for this" — an aggregate — and zero is a leaf.
     */
    fun testHasChildrenFollowsTheServersReference() {
        assertEquals(
            true,
            present(DapVariable("args", "{3 elements}", "cajeta.lang.String[]", 4)).hasChildren,
        )
        assertEquals(
            false,
            present(DapVariable("n", "3", "int32", 0)).hasChildren,
        )
    }

    fun testABlankTypeLeavesTheColumnEmptyRatherThanShowingADot() {
        val node = present(DapVariable(name = "mystery", value = "?", type = "", variablesReference = 0))
        assertNull(node.type)
    }
}
