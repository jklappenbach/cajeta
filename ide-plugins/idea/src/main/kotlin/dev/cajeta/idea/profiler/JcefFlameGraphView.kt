package dev.cajeta.idea.profiler

import com.intellij.ui.jcef.JBCefBrowser
import com.intellij.ui.jcef.JBCefJSQuery
import com.intellij.util.ui.UIUtil
import javax.swing.JComponent

/**
 * cajeta-profiler 11.2.c — the flame graph in Chromium (spec §8.2, §8.6).
 *
 * The same layout as [SwingFlameGraphView] — both call [FlameLayout], so the
 * two surfaces cannot drift about where a frame goes — rendered as HTML so it
 * gets hover, smooth zoom, and real text for free.
 *
 * Frames are absolutely-positioned divs rather than a canvas: a div can be
 * hovered, titled, and selected by the browser without a hit-test of our own,
 * and the layout is already computed. A canvas would mean re-implementing hit
 * testing in JavaScript against numbers Kotlin already has.
 */
class JcefFlameGraphView : FlameGraphView {

    private val browser = JBCefBrowser()
    private val clickQuery = JBCefJSQuery.create(browser as com.intellij.ui.jcef.JBCefBrowserBase)

    private var select: ((FlameNode) -> Unit)? = null
    private var selectLaunch: ((FlameNode) -> Unit)? = null
    private var byId: Map<Int, FlameNode> = emptyMap()

    override val component: JComponent get() = browser.component

    override fun onSelect(handler: (FlameNode) -> Unit) { select = handler }
    override fun onSelectLaunchSite(handler: (FlameNode) -> Unit) { selectLaunch = handler }

    init {
        clickQuery.addHandler { payload ->
            // "<id>:<kind>" — kind distinguishes a plain selection from a
            // request for the launching call site (§8.4).
            val parts = payload.split(':')
            val node = parts.getOrNull(0)?.toIntOrNull()?.let { byId[it] }
            if (node != null) {
                if (parts.getOrNull(1) == "launch") selectLaunch?.invoke(node)
                else select?.invoke(node)
            }
            null
        }
    }

    override fun show(model: ProfileViewModel, track: ProfileTrackView) {
        val (rects, dropped) = FlameLayout.of(model, track)
        byId = rects.withIndex().associate { (i, r) -> i to r.node }
        browser.loadHTML(html(model, track, rects, dropped))
    }

    private fun html(
        model: ProfileViewModel,
        track: ProfileTrackView,
        rects: List<FlameRect>,
        dropped: Int,
    ): String {
        val bg = hex(UIUtil.getPanelBackground())
        val fg = hex(UIUtil.getLabelForeground())
        val muted = hex(UIUtil.getInactiveTextColor())

        val frames = rects.withIndex().joinToString("\n") { (i, r) ->
            val q = model.qualityOf(r.node)
            val colour = FlameColors.cssOf(q, r.node.unclosed)
            val flagged = if (FlameColors.hatched(q)) " flagged" else ""
            // Every reason the measurement is not to be trusted goes in the
            // tooltip. §8.6 asks that a degraded measurement not be presented as
            // equivalent, and saying WHY is what makes that actionable rather
            // than merely decorative.
            val why = buildList {
                add("${r.node.name}")
                add("${fmt(r.node.inclusiveNs)} inclusive, ${fmt(r.node.exclusiveNs)} self")
                model.countsFor(r.node)?.let { add("${it.calls} calls (exact)") }
                q?.let { add("tier: ${it.tier.label}") }
                q?.reasons?.forEach { add("! $it") }
                if (r.node.unclosed) add("! still running when the trace ended")
            }.joinToString("&#10;")

            """<div class="f$flagged" style="left:${pct(r.x)};width:${pct(r.width)};
               top:${r.depth * ROW}px;background:$colour" title="$why"
               data-id="$i">${escape(r.node.name)}</div>"""
        }

        val note = if (dropped > 0)
            """<div class="note">$dropped frame(s) too narrow to draw</div>""" else ""

        return """
<html><head><meta charset="utf-8"><style>
  html,body { margin:0; padding:0; background:$bg; color:$fg;
              font:12px -apple-system,Segoe UI,Ubuntu,sans-serif; }
  #g { position:relative; height:${(rects.maxOfOrNull { it.depth } ?: 0) + 2}00px; }
  .f { position:absolute; height:${ROW - 1}px; overflow:hidden; white-space:nowrap;
       box-sizing:border-box; border:1px solid $bg; padding:0 4px; line-height:${ROW - 1}px;
       cursor:pointer; color:#1a1a1a; }
  .f:hover { filter:brightness(1.15); }
  /* Colour alone would be invisible to a reader with a colour vision
     deficiency, and this is the one distinction §11.3 says must not be missed. */
  .f.flagged { background-image:repeating-linear-gradient(45deg,
      rgba(0,0,0,.28) 0 2px, transparent 2px 6px); }
  .note { color:$muted; padding:6px 4px; }
</style></head><body>
<div id="g">
$frames
</div>
$note
<script>
  document.getElementById('g').addEventListener('click', function (e) {
    var el = e.target.closest('.f');
    if (!el) return;
    var kind = (e.ctrlKey || e.metaKey) ? 'launch' : 'select';
    ${clickQuery.inject("el.dataset.id + ':' + kind")}
  });
  document.getElementById('g').addEventListener('contextmenu', function (e) {
    var el = e.target.closest('.f');
    if (!el) return;
    e.preventDefault();
    ${clickQuery.inject("el.dataset.id + ':launch'")}
  });
</script>
</body></html>""".trimIndent()
    }

    private fun pct(v: Double) = "%.4f%%".format(v * 100.0)

    private fun hex(c: java.awt.Color) = "#%02x%02x%02x".format(c.red, c.green, c.blue)

    private fun fmt(ns: Long): String = when {
        ns >= 1_000_000_000 -> "%.2f s".format(ns / 1e9)
        ns >= 1_000_000 -> "%.2f ms".format(ns / 1e6)
        ns >= 1_000 -> "%.2f us".format(ns / 1e3)
        else -> "$ns ns"
    }

    private fun escape(s: String) = s
        .replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        .replace("\"", "&quot;")

    private companion object { const val ROW = 18 }
}
