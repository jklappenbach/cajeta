package dev.cajeta.idea.profiler

import com.intellij.ui.components.JBScrollPane
import com.intellij.ui.table.JBTable
import java.awt.BorderLayout
import java.awt.Component
import java.awt.event.MouseAdapter
import java.awt.event.MouseEvent
import javax.swing.JPanel
import javax.swing.JTable
import javax.swing.SwingConstants
import javax.swing.table.AbstractTableModel
import javax.swing.table.DefaultTableCellRenderer

/**
 * cajeta-profiler 11.2.f — totals by name, with exact counts when there are any
 * (spec §8.5, §8.7).
 *
 * Ranked by SELF time, not by total. Total time ranks `main` first in every
 * program ever written, which is both true and useless.
 *
 * ## Two columns that refuse to lie
 *
 * **% of track** is blank when the sum behind it is not a duration — a name
 * that ran on several tracks concurrently, or one nested inside itself. §8.7
 * asks that such intervals never be summed into a cost breakdown. The cell says
 * WHY rather than going quietly blank, because an empty cell reads as "zero" or
 * as a bug, and a reader who cannot see the reason will go find the number
 * somewhere less careful.
 *
 * **Calls** appears only when the trace carries instrumentation (§8.5). A
 * permanently empty column on every sampled profile invites the reader to
 * conclude the counts are zero.
 */
class ProfileTotalsPanel : JPanel(BorderLayout()) {

    private var model: ProfileViewModel? = null
    private var rows: List<FlameTotal> = emptyList()
    private var select: ((FlameTotal) -> Unit)? = null

    private val tableModel = object : AbstractTableModel() {
        override fun getRowCount(): Int = rows.size

        override fun getColumnCount(): Int = if (model?.hasInstrumentation == true) 5 else 4

        override fun getColumnName(column: Int): String = when (column) {
            0 -> "Frame"
            1 -> "Self"
            2 -> "Total"
            3 -> "% of track"
            else -> "Calls"
        }

        override fun getValueAt(rowIndex: Int, columnIndex: Int): Any {
            val t = rows[rowIndex]
            return when (columnIndex) {
                0 -> t.name
                1 -> fmt(t.summedExclusiveNs)
                2 -> fmt(t.summedInclusiveNs)
                3 -> fractionCell(t)
                else -> model?.countsFor(t)?.calls?.toString() ?: "—"
            }
        }
    }

    private val table = JBTable(tableModel)

    init {
        table.setShowGrid(false)
        table.autoCreateRowSorter = false   // the ranking is the model's, not the header's
        val right = object : DefaultTableCellRenderer() {
            init { horizontalAlignment = SwingConstants.RIGHT }
            override fun getTableCellRendererComponent(
                t: JTable?, value: Any?, sel: Boolean, focus: Boolean, r: Int, c: Int,
            ): Component {
                val comp = super.getTableCellRendererComponent(t, value, sel, focus, r, c)
                if (c == 3) toolTipText = rows.getOrNull(r)?.let { whyNoFraction(it) }
                return comp
            }
        }
        for (c in 1 until tableModel.columnCount) {
            table.columnModel.getColumn(c).cellRenderer = right
        }
        table.addMouseListener(object : MouseAdapter() {
            override fun mouseClicked(e: MouseEvent) {
                if (e.clickCount < 2) return
                rows.getOrNull(table.rowAtPoint(e.point))?.let { select?.invoke(it) }
            }
        })
        add(JBScrollPane(table), BorderLayout.CENTER)
    }

    fun onSelect(handler: (FlameTotal) -> Unit) { select = handler }

    fun show(model: ProfileViewModel) {
        this.model = model
        rows = model.totals
        tableModel.fireTableStructureChanged()
    }

    // --- §8.7, rendered ----------------------------------------------------------

    private fun fractionCell(t: FlameTotal): String {
        val f = t.wallClockFraction ?: return when {
            t.concurrent && t.recursive -> "— concurrent, recursive"
            t.concurrent -> "— ${t.byTrack.size} tracks"
            t.recursive -> "— recursive"
            else -> "—"
        }
        return "%.1f%%".format(f * 100.0)
    }

    private fun whyNoFraction(t: FlameTotal): String? {
        if (t.wallClockFraction != null) return null
        return buildString {
            append("Summing these spans would not give a duration.")
            if (t.concurrent) append(
                "\nIt ran on ${t.byTrack.size} tracks at once, so the spans overlap in wall time.")
            if (t.recursive) append(
                "\nIt is nested inside itself, so its time is counted once per level.")
            append("\nSelf time is unaffected: each nanosecond is charged to one frame.")
        }
    }

    private fun fmt(ns: Long): String = when {
        ns >= 1_000_000_000 -> "%.3f s".format(ns / 1e9)
        ns >= 1_000_000 -> "%.2f ms".format(ns / 1e6)
        ns >= 1_000 -> "%.2f us".format(ns / 1e3)
        else -> "$ns ns"
    }
}
