package dev.cajeta.idea.debugger

import com.intellij.icons.AllIcons
import com.intellij.xdebugger.frame.XValue
import com.intellij.xdebugger.frame.XValueNode
import com.intellij.xdebugger.frame.XValuePlace

/**
 * A single local variable in the Variables view (CP6d). The cajeta server emits
 * leaf values only (variablesReference == 0), so there are no children yet;
 * nested-object expansion would recurse through [CajetaDebugSession.variables]
 * here once the server returns non-zero references. Value edit is CP6e.
 */
class CajetaValue(private val variable: DapVariable) : XValue() {

    override fun computePresentation(node: XValueNode, place: XValuePlace) {
        node.setPresentation(
            AllIcons.Debugger.Value,
            variable.type.ifBlank { null },
            variable.value,
            variable.variablesReference != 0, // hasChildren
        )
    }
}
