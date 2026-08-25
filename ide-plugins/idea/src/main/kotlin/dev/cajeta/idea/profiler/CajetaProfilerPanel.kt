package dev.cajeta.idea.profiler

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.ui.ComboBox
import com.intellij.ui.components.JBLabel
import com.intellij.ui.components.JBTabbedPane
import java.awt.BorderLayout
import java.awt.FlowLayout
import java.io.File
import javax.swing.DefaultComboBoxModel
import javax.swing.JPanel

/**
 * cajeta-profiler 11.2.e — the profiler tool window's contents (spec §8.1-§8.7).
 *
 * One window, a tab per view, following the coco window's precedent: the flame
 * graph, the timeline and the totals are one run looked at three ways, so they
 * share a place and an open/closed state.
 *
 * The track selector belongs to the flame graph and not to the timeline —
 * a flame graph shows ONE lane at a time (a merged one would stack unrelated
 * stacks on a shared x-axis and invite the reader to compare them), while the
 * timeline exists precisely to show every lane at once.
 */
class CajetaProfilerPanel(private val project: Project) : JPanel(BorderLayout()) {

    private val flame = FlameGraphViewFactory.create()
    private val timeline = ProfileTimelinePanel()
    private val totals = ProfileTotalsPanel()

    private val trackPicker = ComboBox<ProfileTrackView>()
    private val status = JBLabel(" ")
    private val tabs = JBTabbedPane()

    private var model: ProfileViewModel? = null

    init {
        val bar = JPanel(FlowLayout(FlowLayout.LEFT, 8, 4))
        bar.add(JBLabel("Track:"))
        bar.add(trackPicker)
        bar.add(status)

        trackPicker.renderer = com.intellij.ui.SimpleListCellRenderer.create("") { t: ProfileTrackView? ->
            t?.let { "${it.name}  (${it.kind.name.lowercase()}, depth ${it.depth})" } ?: ""
        }
        trackPicker.addActionListener {
            val m = model ?: return@addActionListener
            (trackPicker.selectedItem as? ProfileTrackView)?.let { flame.show(m, it) }
        }

        val flamePane = JPanel(BorderLayout())
        flamePane.add(bar, BorderLayout.NORTH)
        flamePane.add(flame.component, BorderLayout.CENTER)

        tabs.addTab("Flame Graph", flamePane)
        tabs.addTab("Timeline", timeline)
        tabs.addTab("Totals", totals)
        add(tabs, BorderLayout.CENTER)

        // §8.2 — selecting a frame navigates to its source.
        flame.onSelect { node -> ProfileNavigation.open(project, node) }
        timeline.onSelect { node -> ProfileNavigation.open(project, node) }

        // §8.4 — and a kernel reaches the line that launched it, which is a
        // DIFFERENT place from the kernel's own location.
        flame.onSelectLaunchSite { node ->
            val m = model
            if (m != null && !m.launchSites.open(project, node)) {
                // Falling through to the frame's own location would silently
                // answer a different question than the one asked.
                status.text = "no recorded launch site for ${node.name}"
            }
        }

        totals.onSelect { total ->
            val m = model ?: return@onSelect
            m.tracks.asSequence()
                .flatMap { it.roots.asSequence() }
                .firstNotNullOfOrNull { find(it, total.name) }
                ?.let { ProfileNavigation.open(project, it) }
        }
    }

    private fun find(node: FlameNode, name: String): FlameNode? {
        if (node.name == name) return node
        for (c in node.children) find(c, name)?.let { return it }
        return null
    }

    /** Load a trace. Reading and decoding happen off the EDT. */
    fun load(file: File) {
        status.text = "reading ${file.name}…"
        ApplicationManager.getApplication().executeOnPooledThread {
            val result = runCatching { ProfileViewModel.of(PerfettoTraceReader.read(file.readBytes())) }
            ApplicationManager.getApplication().invokeLater {
                result.onSuccess { show(it, file) }
                    .onFailure { status.text = "could not read ${file.name}: ${it.message}" }
            }
        }
    }

    fun show(model: ProfileViewModel, file: File? = null) {
        this.model = model
        totals.show(model)
        timeline.show(model)

        val lanes = model.workTracks
        trackPicker.model = DefaultComboBoxModel(lanes.toTypedArray())
        // The busiest lane, so the window opens on something worth looking at
        // rather than on whichever track the writer happened to emit first.
        val initial = lanes.maxByOrNull { t -> t.roots.sumOf { it.inclusiveNs } }
        if (initial != null) {
            trackPicker.selectedItem = initial
            flame.show(model, initial)
        }
        status.text = summary(model, file)
    }

    private fun summary(m: ProfileViewModel, file: File?): String = buildString {
        file?.let { append(it.name).append("  ") }
        append("${m.workTracks.size} track(s), ${fmtNs(m.spanNs)}")
        if (m.hasDeviceWork) append(", device work")
        if (m.hasInstrumentation) append(", ${m.counts.size} instrumented method(s)")
        // §7.7 — a trace cut short is still worth reading, and the reader is
        // told rather than left to wonder why the tail looks thin.
        if (m.truncated) append("  —  TRUNCATED: the last packet was incomplete")
    }

    private fun fmtNs(ns: Long): String = when {
        ns >= 1_000_000_000 -> "%.2f s".format(ns / 1e9)
        ns >= 1_000_000 -> "%.1f ms".format(ns / 1e6)
        else -> "$ns ns"
    }
}
