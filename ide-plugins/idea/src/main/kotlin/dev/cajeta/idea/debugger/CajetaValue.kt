package dev.cajeta.idea.debugger

import com.intellij.icons.AllIcons
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors
import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.xdebugger.XExpression
import com.intellij.xdebugger.frame.XValue
import com.intellij.xdebugger.frame.XValueModifier
import com.intellij.xdebugger.frame.XValueNode
import com.intellij.xdebugger.frame.XValuePlace
import com.intellij.xdebugger.frame.presentation.XValuePresentation
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
        if (!facets.isKnown) {
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
