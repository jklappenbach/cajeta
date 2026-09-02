package dev.cajeta.idea.profiler

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.ui.ComboBox
import com.intellij.ui.components.JBLabel
import com.intellij.ui.components.JBTabbedPane
import java.awt.BorderLayout
import java.awt.CardLayout
import java.awt.FlowLayout
import java.awt.GridBagConstraints
import java.awt.GridBagLayout
import java.io.File
import javax.swing.DefaultComboBoxModel
import javax.swing.JButton
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
    // The loaded profile's identity, which is STANDING information. It shared
    // the status label with transient navigation messages, so the first click
    // erased "which profile am I looking at" and nothing put it back.
    private val profileLabel = JBLabel(" ")
    private val tabs = JBTabbedPane()

    // §5.4 — a window holding nothing must say so and say what to do about it.
    // It used to show bare tabs: no trace, no explanation, no way to get one.
    private val cards = CardLayout()
    private val deck = JPanel(cards)

    private var model: ProfileViewModel? = null

    init {
        // Two rows, each with one job:
        //   1. what is loaded (left)          | which track (right)
        //   2. zoom (left)                    | what the last click did
        // The profile's identity and a transient message were previously the
        // same label, so clicking anything erased which file was open.
        val rowOne = JPanel(BorderLayout())
        val profileCell = JPanel(FlowLayout(FlowLayout.LEFT, 8, 4))
        profileCell.add(profileLabel)
        rowOne.add(profileCell, BorderLayout.CENTER)
        val trackCell = JPanel(FlowLayout(FlowLayout.RIGHT, 8, 4))
        trackCell.add(JBLabel("Track:"))
        trackCell.add(trackPicker)
        rowOne.add(trackCell, BorderLayout.EAST)

        val flameZoom = ZoomSlider { z -> flame.setZoom(z) }
        // Ctrl+scroll on the graph moves the slider, so the two never disagree.
        flame.onZoomChanged { z -> flameZoom.reflect(z) }
        val rowTwo = JPanel(BorderLayout())
        rowTwo.add(flameZoom, BorderLayout.WEST)
        val statusCell = JPanel(FlowLayout(FlowLayout.LEFT, 8, 4))
        statusCell.add(status)
        rowTwo.add(statusCell, BorderLayout.CENTER)

        val bar = JPanel(java.awt.GridLayout(2, 1))
        bar.add(rowOne)
        bar.add(rowTwo)

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

        deck.add(buildEmptyState(), EMPTY)
        deck.add(tabs, LOADED)
        add(deck, BorderLayout.CENTER)
        cards.show(deck, if (ProfilerEmptyState.shouldShow(model)) EMPTY else LOADED)

        // §8.2 — selecting a frame navigates to its source, and SAYS what
        // happened when it does not. The Boolean was dropped here, so a frame
        // that resolved nowhere did nothing and reported nothing, and one that
        // resolved without a line opened at the top of the file — which reads
        // as a dead click (reported three ways on 2026-08-31).
        flame.onSelect { node -> navigate(node) }
        timeline.onSelect { node -> navigate(node) }

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

        // §8.2 again, on the third surface. This dropped the Boolean exactly as
        // the flame graph did, so a totals row that reached nowhere was
        // indistinguishable from one that was not clickable at all — which is
        // how it was reported ("clickable by tour, not by cajeta", 2026-09-01).
        // Routed through the same navigate() so all three views answer alike.
        // A totals row names a METHOD, so it navigates to the method — not to
        // whatever line a sampler happened to catch. Every frame in a trace
        // resolves past its own declaration (measured: all 53 in the tour
        // profile, median 6 lines in, worst 146), and for an aggregate of every
        // occurrence across every track that line is arbitrary.
        totals.onSelect { total ->
            val m = model ?: return@onSelect
            if (TotalsNavigation.open(project, total.name)) {
                status.text = ""
                return@onSelect
            }
            // No indexed declaration. The trace's own line still gets the
            // reader close, but it is mid-method by construction, so say so
            // rather than leaving them to wonder why it landed there.
            val node = m.tracks.asSequence()
                .flatMap { it.roots.asSequence() }
                .firstNotNullOfOrNull { find(it, total.name) }
            if (node == null) {
                // A totals row is aggregated from frames, so this is rare — the
                // profiler's own run record is one. Saying so beats a dead row.
                status.text = "no frame in this trace is named ${total.name}"
            } else {
                navigate(node)
                if (status.text.isEmpty()) {
                    status.text = "no indexed declaration for ${total.name} — " +
                        "this is the line the sampler observed, not the start of the method"
                }
            }
        }
    }

    /** The empty state: what it is, and the same chooser the Tools action uses. */
    private fun buildEmptyState(): JPanel {
        val panel = JPanel(GridBagLayout())
        val column = JPanel()
        column.layout = javax.swing.BoxLayout(column, javax.swing.BoxLayout.Y_AXIS)
        column.add(JBLabel(ProfilerEmptyState.TITLE))
        column.add(JBLabel(ProfilerEmptyState.message()))
        column.add(JButton("Open Profile…").apply {
            addActionListener {
                CajetaProfileLocation.choose(project)?.let { load(it) }
            }
        })
        panel.add(column, GridBagConstraints())
        return panel
    }

    private fun navigate(node: FlameNode) {
        val opened = ProfileNavigation.open(project, node)
        status.text = NavigationOutcome.describe(
            frame = node.name,
            location = node.sourceLocation,
            opened = opened,
            exact = if (opened) ProfileNavigation.lastExact else false,
            // Asked only on failure, and memoized, so this does not spawn a
            // process on every click.
            stdlibMounted = if (opened) true else ProfileNavigation.stdlibAvailable(),
        )
    }

    private fun find(node: FlameNode, name: String): FlameNode? {
        if (node.name == name) return node
        for (c in node.children) find(c, name)?.let { return it }
        return null
    }

    /** Load a trace. Reading and decoding happen off the EDT. */
    fun load(file: File) {
        profileLabel.text = "Profile: reading ${file.name}…"
        ApplicationManager.getApplication().executeOnPooledThread {
            val result = runCatching { ProfileViewModel.of(PerfettoTraceReader.read(file.readBytes())) }
            ApplicationManager.getApplication().invokeLater {
                result.onSuccess { show(it, file) }
                    .onFailure {
                        profileLabel.text = "Profile: could not read ${file.name}: ${it.message}"
                    }
            }
        }
    }

    fun show(model: ProfileViewModel, file: File? = null) {
        this.model = model
        cards.show(deck, LOADED)
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
        profileLabel.text = "Profile: " + summary(model, file)
        status.text = " "
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

    private companion object {
        const val EMPTY = "empty"
        const val LOADED = "loaded"
    }
}
