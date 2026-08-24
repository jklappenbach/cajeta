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
    /**
     * Flows this frame ends — for a device slice, the launch that caused it
     * (spec §8.4). Carried onto the node because the UI selects nodes, not raw
     * events, and a flow that stops at the tree build leaves §8.4 unreachable.
     */
    val terminatingFlowIds: List<Long> = emptyList(),
    /**
     * The begin event's debug annotations — tier, clock confidence, integrity
     * flags (§8.6). Carried here so a selected node can be judged without
     * holding the raw event beside it.
     */
    val annotations: Map<String, String> = emptyMap(),
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
 * [summedInclusiveNs] is a sum of spans, and there are two ways such a sum
 * stops being a duration:
 *
 * **Concurrency** ([concurrent]) — spans on different tracks overlap in wall
 * time. In the tour fixture `ParallelDriver.findFailWorker` sums to 189 ms
 * across four fibers inside a run whose wall clock is 65 ms.
 *
 * **Recursion** ([recursive]) — a frame nested inside another occurrence of
 * itself has its time counted once per level. `walk` three deep over 500 ns
 * sums to 900 ns. This one hides on a single track, where the concurrency test
 * does not fire, and it is why that test alone was not enough.
 *
 * Either way the sum is a true statement about work done and a false one about
 * elapsed time, so [wallClockFraction] is null. §8.7 asks that non-additive
 * intervals never be summed into a cost breakdown; the breakdown by track is
 * what remains sayable and is kept rather than the number being hidden, and the
 * two booleans say WHICH reason applies so the UI can explain itself instead of
 * silently omitting a column.
 *
 * [summedExclusiveNs] stays additive under both: each nanosecond is charged to
 * exactly one frame, so it is what a cost ranking should use.
 */
data class FlameTotal(
    val name: String,
    val occurrences: Int,
    val summedInclusiveNs: Long,
    val summedExclusiveNs: Long,
    val byTrack: Map<Long, Long>,
    val wallClockFraction: Double?,
    /** Occurrences on more than one track, which run concurrently. */
    val concurrent: Boolean = false,
    /** At least one occurrence is nested inside another occurrence of itself. */
    val recursive: Boolean = false,
)

object FlameGraph {

    /**
     * Build one forest per track.
     *
     * Events are consumed in timestamp order under a STABLE sort, so equal
     * timestamps keep their emission order. Both halves matter: `main`,
     * `forEach` and the lambda beneath them all begin at the same nanosecond in
     * the tour fixture, and only emission order carries that nesting — while
     * the GPU writer emits each device slice's BEGIN and END as an adjacent
     * pair, so on a queue only timestamps carry the nesting and file order
     * would flatten every slice to a root.
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
        val terminatingFlowIds: List<Long>,
        val annotations: Map<String, String>,
    ) {
        val children = ArrayList<FlameNode>()
    }

    private fun buildTrack(uuid: Long, events: List<ProfileEvent>): List<FlameNode> {
        val ordered = events.sortedBy { it.timestampNs }
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
                terminatingFlowIds = open.terminatingFlowIds,
                annotations = open.annotations,
            )
            if (stack.isEmpty()) roots.add(node) else stack.last().children.add(node)
        }

        for (e in ordered) {
            if (e.timestampNs > 0) lastTs = e.timestampNs
            when {
                e.isBegin -> stack.addLast(
                    Open(e.name ?: "", e.timestampNs, e.sourceLocation,
                        e.terminatingFlowIds, e.annotations))
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
                        terminatingFlowIds = e.terminatingFlowIds,
                        annotations = e.annotations,
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
            var recursive: Boolean = false,
        )

        val acc = LinkedHashMap<String, Acc>()
        val spans = tracks.associate { it.track.uuid to it.spanNs }

        // Names currently on the walk's own stack. A name already present when
        // we reach it again is nested inside itself, and its inclusive time is
        // about to be counted for a second time.
        val onStack = HashSet<String>()

        fun visit(n: FlameNode) {
            val a = acc.getOrPut(n.name) { Acc() }
            a.occurrences++
            a.inclusive += n.inclusiveNs
            a.exclusive += n.exclusiveNs
            a.perTrack[n.trackUuid] = (a.perTrack[n.trackUuid] ?: 0) + n.inclusiveNs

            // Mutual recursion counts too: a -> b -> a nests `a` inside itself
            // just as directly as a -> a, and costs the same double count.
            val fresh = onStack.add(n.name)
            if (!fresh) a.recursive = true
            n.children.forEach(::visit)
            if (fresh) onStack.remove(n.name)
        }
        tracks.flatMap { it.roots }.forEach(::visit)

        return acc.map { (name, a) ->
            val concurrent = a.perTrack.size > 1
            // A fraction is offered only when the sum is genuinely a duration:
            // one track, and no occurrence nested inside another (§8.7).
            //
            // Concurrency and recursion are separate failures and neither
            // implies the other — findFailWorker is concurrent and not
            // recursive; a self-nested frame on a single thread is the reverse.
            val fraction = if (concurrent || a.recursive) null else {
                val span = spans[a.perTrack.keys.first()] ?: 0
                if (span > 0) a.inclusive.toDouble() / span.toDouble() else null
            }
            FlameTotal(
                name, a.occurrences, a.inclusive, a.exclusive, a.perTrack, fraction,
                concurrent = concurrent, recursive = a.recursive,
            )
        }.sortedByDescending { it.summedExclusiveNs }
    }
}
