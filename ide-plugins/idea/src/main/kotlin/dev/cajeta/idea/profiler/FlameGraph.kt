package dev.cajeta.idea.profiler

/**
 * cajeta-profiler Unit 11 — the call tree, and what it costs (spec §8.2, §8.7).
 *
 * One node per frame the trace recorded.
 *
 * `inclusiveNs` is the frame's own span; `exclusiveNs` is that span less what
 * its direct children took, which is the time the frame actually spent doing
 * its own work. Reporting inclusive time as a frame's cost is the single most
 * misleading thing a flame graph can do — `main` accounts for the entire run by
 * that measure, in every program ever written.
 */
data class FlameNode(
    val name: String,
    val trackUuid: Long,
    val startNs: Long,
    val inclusiveNs: Long,
    val exclusiveNs: Long,
    val children: List<FlameNode>,
    /** Where to navigate when this frame is selected (spec §8.2). */
    val sourceLocation: ProfileSourceLocation?,
    /** The frame was still running when the trace ended. */
    val unclosed: Boolean = false,
) {
    val depth: Int get() = 1 + (children.maxOfOrNull { it.depth } ?: 0)
}

/** One timeline lane's forest: a thread, a fiber, or a device queue (§8.3). */
data class FlameTrack(
    val track: ProfileTrack,
    val roots: List<FlameNode>,
    /** First begin to last end on this track. */
    val spanNs: Long,
)

/**
 * A name's total across the trace.
 *
 * [summedInclusiveNs] is a sum of spans, and spans on different tracks overlap
 * in wall time. In the tour fixture `ParallelDriver.findFailWorker` sums to
 * 189 ms across four fibers inside a run whose wall clock is 65 ms. The sum is
 * a true statement about work done and a false one about elapsed time.
 *
 * So [wallClockFraction] is null whenever the occurrences span more than one
 * track. §8.7 asks that non-additive intervals never be summed into a cost
 * breakdown; the breakdown by track is what remains sayable, and it is kept
 * rather than the number being hidden.
 */
data class FlameTotal(
    val name: String,
    val occurrences: Int,
    val summedInclusiveNs: Long,
    val summedExclusiveNs: Long,
    val byTrack: Map<Long, Long>,
    val wallClockFraction: Double?,
)

object FlameGraph {

    /**
     * Build one forest per track.
     *
     * Events are consumed in FILE order, never sorted by timestamp. Nesting is
     * carried by emission order: `main`, `forEach` and the lambda beneath them
     * all begin at the same nanosecond in the tour fixture, and sorting by
     * timestamp would flatten three real levels into an arbitrary order while
     * still producing a tree that looked entirely reasonable.
     */
    fun build(trace: ProfileTrace): List<FlameTrack> {
        if (trace.events.isEmpty()) return emptyList()

        val byTrack = LinkedHashMap<Long, MutableList<ProfileEvent>>()
        for (e in trace.events) byTrack.getOrPut(e.trackUuid) { ArrayList() }.add(e)

        val named = trace.tracks.associateBy { it.uuid }
        val out = ArrayList<FlameTrack>()
        for ((uuid, events) in byTrack) {
            val track = named[uuid] ?: ProfileTrack(uuid, "track.$uuid")
            val roots = buildTrack(uuid, events)
            val first = events.firstOrNull { it.timestampNs > 0 }?.timestampNs ?: 0
            val last = events.lastOrNull { it.timestampNs > 0 }?.timestampNs ?: first
            out.add(FlameTrack(track, roots, (last - first).coerceAtLeast(0)))
        }
        // Declaration order, so the host thread stays above its fibers rather
        // than wherever a hash landed it.
        return out.sortedBy { t -> trace.tracks.indexOfFirst { it.uuid == t.track.uuid } }
    }

    /** A frame under construction: children accumulate until its END arrives. */
    private class Open(
        val name: String,
        val startNs: Long,
        val sourceLocation: ProfileSourceLocation?,
    ) {
        val children = ArrayList<FlameNode>()
    }

    private fun buildTrack(uuid: Long, events: List<ProfileEvent>): List<FlameNode> {
        val stack = ArrayDeque<Open>()
        val roots = ArrayList<FlameNode>()
        var lastTs = 0L

        fun close(open: Open, endNs: Long, unclosed: Boolean) {
            val inclusive = (endNs - open.startNs).coerceAtLeast(0)
            val childTotal = open.children.sumOf { it.inclusiveNs }
            val node = FlameNode(
                name = open.name,
                trackUuid = uuid,
                startNs = open.startNs,
                inclusiveNs = inclusive,
                // Clamped at zero. A child that outlives its parent is a broken
                // trace, and a negative cost renders as a bar pointing the
                // wrong way rather than as an error anyone would notice.
                exclusiveNs = (inclusive - childTotal).coerceAtLeast(0),
                children = open.children.toList(),
                sourceLocation = open.sourceLocation,
                unclosed = unclosed,
            )
            if (stack.isEmpty()) roots.add(node) else stack.last().children.add(node)
        }

        for (e in events) {
            if (e.timestampNs > 0) lastTs = e.timestampNs
            when {
                e.isBegin -> stack.addLast(Open(e.name ?: "", e.timestampNs, e.sourceLocation))
                e.isEnd -> {
                    // An END with nothing open is a trace that began mid-slice.
                    // Dropping it is right; failing on it would refuse a file
                    // that is merely incomplete at the front.
                    val open = stack.removeLastOrNull() ?: continue
                    close(open, e.timestampNs, unclosed = false)
                }
                e.isInstant -> {
                    val node = FlameNode(
                        name = e.name ?: "",
                        trackUuid = uuid,
                        startNs = e.timestampNs,
                        inclusiveNs = 0,
                        exclusiveNs = 0,
                        children = emptyList(),
                        sourceLocation = e.sourceLocation,
                    )
                    if (stack.isEmpty()) roots.add(node) else stack.last().children.add(node)
                }
            }
        }

        // Whatever is still open was executing when the trace ended — which
        // makes those frames the most interesting ones in a trace from a
        // process that hung or was killed. They are closed at the last
        // timestamp seen and marked, never dropped.
        while (stack.isNotEmpty()) close(stack.removeLast(), lastTs, unclosed = true)
        return roots
    }

    /** Totals per name across every track. See [FlameTotal] on the wall-clock rule. */
    fun byName(tracks: List<FlameTrack>): List<FlameTotal> {
        data class Acc(
            var occurrences: Int = 0,
            var inclusive: Long = 0,
            var exclusive: Long = 0,
            val perTrack: LinkedHashMap<Long, Long> = LinkedHashMap(),
        )

        val acc = LinkedHashMap<String, Acc>()
        val spans = tracks.associate { it.track.uuid to it.spanNs }

        fun visit(n: FlameNode) {
            val a = acc.getOrPut(n.name) { Acc() }
            a.occurrences++
            a.inclusive += n.inclusiveNs
            a.exclusive += n.exclusiveNs
            a.perTrack[n.trackUuid] = (a.perTrack[n.trackUuid] ?: 0) + n.inclusiveNs
            n.children.forEach(::visit)
        }
        tracks.flatMap { it.roots }.forEach(::visit)

        return acc.map { (name, a) ->
            // One track: the sum is a genuine share of that track's span.
            // More than one: the tracks run concurrently and the sum is not a
            // duration at all, so no fraction is offered (§8.7).
            val fraction = if (a.perTrack.size == 1) {
                val uuid = a.perTrack.keys.first()
                val span = spans[uuid] ?: 0
                if (span > 0) a.inclusive.toDouble() / span.toDouble() else null
            } else null
            FlameTotal(name, a.occurrences, a.inclusive, a.exclusive, a.perTrack, fraction)
        }.sortedByDescending { it.summedExclusiveNs }
    }
}
