package dev.cajeta.idea.debugger

import com.intellij.icons.AllIcons
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors
import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.xdebugger.XExpression
import com.intellij.xdebugger.frame.XValue
import com.intellij.xdebugger.frame.XCompositeNode
import com.intellij.xdebugger.frame.XValueChildrenList
import com.intellij.xdebugger.frame.XValueModifier
import com.intellij.xdebugger.frame.XValueNode
import com.intellij.xdebugger.frame.XValuePlace
import com.intellij.xdebugger.frame.presentation.XValuePresentation
import dev.cajeta.idea.settings.CajetaSettings
import javax.swing.Icon

/**
 * Renders a single DAP variable as an XValue in the IntelliJ Variables view.
 * CP6d: leaf rendering (name : type = value). CP6e: in-place value editing via
 * setVariable. CP7-2: memory-facet decoration — ownership picks the icon,
 * allocation class tints the value, a moved-out binding is rendered as an error
 * (consumed, must not read as a live value, FR-4.3), and a compact facet tag is
 * appended as a comment (the color-independent textual affordance, FR-5.3). The
 * facets→presentation decision is the platform-free [present]; this class only
 * translates that descriptor into icons/attributes.
 */
class CajetaValue(
    private val variable: DapVariable,
    private val session: CajetaDebugSession? = null,
) : XValue() {

    override fun computePresentation(node: XValueNode, place: XValuePlace) {
        val facets = variable.facets
        // CP7-5 (FR-7.3): the Variables-view encoding is independently toggleable.
        if (!facets.isKnown || !CajetaSettings.instance.showFacetsInVariables) {
            // No metadata (non-cajeta or undetermined): the original plain leaf.
            node.setPresentation(
                AllIcons.Debugger.Value,
                variable.type.ifBlank { null },
                variable.value,
                variable.variablesReference != 0,
            )
            return
        }

        val pres = facets.present()
        val icon = ownershipIcon(facets.ownership)
        val valueColor = allocColor(facets.alloc)
        val typeText = variable.type.ifBlank { null }
        val hasChildren = variable.variablesReference != 0

        node.setPresentation(icon, object : XValuePresentation() {
            override fun getType(): String? = typeText

            override fun renderValue(renderer: XValueTextRenderer) {
                if (pres.error) {
                    // Moved-out: signal "do not trust this value" distinctly.
                    renderer.renderError(variable.value)
                } else if (valueColor != null) {
                    renderer.renderValue(variable.value, valueColor)
                } else {
                    renderer.renderValue(variable.value)
                }
                if (pres.tag.isNotEmpty()) {
                    renderer.renderComment("  " + pres.tag)
                }
            }
        }, hasChildren)
    }

    /**
     * Expand an aggregate (array element rows, object fields, or a collection's
     * logical elements/entries) by fetching `variables(variablesReference)` from
     * the server — the same call the frame scope uses, just keyed by this value's
     * reference instead of a scope's. Each child is tagged with THIS value's
     * reference as its container so editing a scalar leaf targets it via
     * setVariable. A leaf (reference 0) or a detached value (no session) has no
     * children. Paging arrives as a synthetic "[N more…]" child that is itself
     * expandable, so it needs no special handling here.
     */
    override fun computeChildren(node: XCompositeNode) {
        val ds = session
        val ref = variable.variablesReference
        if (ds == null || ref == 0) {
            node.addChildren(XValueChildrenList.EMPTY, true)
            return
        }
        ds.variables(ref).thenAccept { response ->
            val children = XValueChildrenList()
            for (v in CajetaDebugSession.parseVariables(response)) {
                children.add(v.name, CajetaValue(v.copy(containerReference = ref), ds))
            }
            node.addChildren(children, true)
        }.exceptionally {
            node.setErrorMessage("Failed to load children: ${it.message}")
            null
        }
    }

    /**
     * Editable only when we have a session and a containing scope to target,
     * and the binding is not moved out (a consumed binding is read-only — the
     * server marks it presentationHint.readOnly; CP7-2 mirrors that here).
     */
    override fun getModifier(): XValueModifier? {
        val ds = session ?: return null
        if (variable.containerReference == 0) return null
        if (variable.facets.lifetime == LifetimeState.MOVED_OUT) return null
        return object : XValueModifier() {
            override fun getInitialValueEditorText(): String = variable.value

            override fun setValue(expression: XExpression, callback: XModificationCallback) {
                ds.setVariable(variable.containerReference, variable.name, expression.expression)
                    .thenAccept { callback.valueModified() }
                    .exceptionally { callback.errorOccurred(it.message ?: "set failed"); null }
            }
        }
    }

    private fun ownershipIcon(role: OwnershipRole): Icon = when (role) {
        OwnershipRole.OWNER -> AllIcons.Nodes.Field
        OwnershipRole.BORROW -> AllIcons.Nodes.Parameter
        OwnershipRole.MOVED -> AllIcons.Nodes.Field
        OwnershipRole.UNKNOWN -> AllIcons.Debugger.Value
    }

    private fun allocColor(alloc: AllocClass): TextAttributesKey? = when (alloc) {
        AllocClass.STACK -> DefaultLanguageHighlighterColors.LOCAL_VARIABLE
        AllocClass.HEAP -> DefaultLanguageHighlighterColors.INSTANCE_FIELD
        AllocClass.SHARED -> DefaultLanguageHighlighterColors.STATIC_FIELD
        AllocClass.UNKNOWN -> null
    }
}
