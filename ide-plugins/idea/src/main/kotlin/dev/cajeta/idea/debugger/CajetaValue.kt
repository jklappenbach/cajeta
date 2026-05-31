package dev.cajeta.idea.debugger

import com.intellij.icons.AllIcons
import com.intellij.xdebugger.XExpression
import com.intellij.xdebugger.frame.XValue
import com.intellij.xdebugger.frame.XValueModifier
import com.intellij.xdebugger.frame.XValueNode
import com.intellij.xdebugger.frame.XValuePlace

/**
 * Renders a single DAP variable as an XValue in the IntelliJ Variables view.
 * CP6d: leaf rendering (name : type = value). CP6e: in-place value editing via
 * setVariable, wired through the plain-core session.
 */
class CajetaValue(
    private val variable: DapVariable,
    private val session: CajetaDebugSession? = null,
) : XValue() {

    override fun computePresentation(node: XValueNode, place: XValuePlace) {
        node.setPresentation(
            AllIcons.Debugger.Value,
            variable.type.ifBlank { null },
            variable.value,
            variable.variablesReference != 0, // hasChildren
        )
    }

    /**
     * Editable only when we have a session and a containing scope to target.
     * The actual write goes through CajetaDebugSession.setVariable; the server
     * writes the live fiber's stack slot and returns the re-rendered value.
     */
    override fun getModifier(): XValueModifier? {
        val ds = session ?: return null
        if (variable.containerReference == 0) return null
        return object : XValueModifier() {
            override fun getInitialValueEditorText(): String = variable.value

            override fun setValue(expression: XExpression, callback: XModificationCallback) {
                ds.setVariable(variable.containerReference, variable.name, expression.expression)
                    .thenAccept { callback.valueModified() }
                    .exceptionally { callback.errorOccurred(it.message ?: "set failed"); null }
            }
        }
    }
}
