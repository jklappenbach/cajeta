package dev.cajeta.idea.profiler

/**
 * cajeta-profiler 11.2.b — packets, mapped to what the views render
 * (spec §8.2, §8.3, §8.5, §8.6).
 *
 * The reader answers "what does this file say". This answers "what do we draw".
 * Everything a panel needs is computed once, here, so the flame graph and the
 * timeline agree about the run rather than each deriving its own version of it.
 *
 * ## One time axis
 *
 * §8.3 asks for host threads, fibers and device queues on ONE axis. That is a
 * property of the model, not of a renderer: [startNs] and [endNs] span the whole
 * run, and every track is placed against them. A view that scaled each track to
 * its own extent would draw a device queue that was busy for 3% of the run as a
 * full-width bar, which is the exact misreading a merged timeline exists to
 * prevent.
 */

/** What a track represents. Named from the writer's own track names. */
enum class TrackKind {
    /** An OS thread the sampler saw. */
    THREAD,

    /** A fiber, distinct from its carrier thread (spec §4.3). */
    FIBER,

    /** A device execution queue — where kernels actually ran. */
    DEVICE_QUEUE,

    /** A GPU device or its context: structure, with no slices of its own. */
    DEVICE,

    /** Per-method exact counts (§3.4, §8.5). Instants, not spans. */
    INSTRUMENTATION,

    /** The profiler's own run record (§7.8). */
    PROFILER,

    OTHER;

    /** Tracks that carry program work, as opposed to structure or metadata. */
    val carriesWork: Boolean
        get() = this == THREAD || this == FIBER || this == DEVICE_QUEUE

    companion object {
        fun of(name: String): TrackKind = when {
            name.startsWith("cajeta.thread.") -> THREAD
            name.startsWith("cajeta.fiber.") -> FIBER
            name.startsWith("queue ") -> DEVICE_QUEUE
            name.startsWith("cajeta.xpu.") || name.startsWith("context ") -> DEVICE
            name == "cajeta.instrumentation" -> INSTRUMENTATION
            name == "cajeta.profiler" -> PROFILER
            else -> OTHER
        }
    }
}

data class ProfileTrackView(
    val track: ProfileTrack,
    val kind: TrackKind,
    val roots: List<FlameNode>,
    val startNs: Long,
    val endNs: Long,
) {
    val name: String get() = track.name
    val spanNs: Long get() = (endNs - startNs).coerceAtLeast(0)
    val depth: Int get() = roots.maxOfOrNull { it.depth } ?: 0
    val isEmpty: Boolean get() = roots.isEmpty()
}

/**
 * Exact per-method counts from an instrumented build (§3.4, §8.5).
 *
 * [outsideSelectionCalls] is §3.11: entries reached with no probed frame beneath
 * them. It is kept because the alternative — attributing them to the nearest
 * probed ancestor — fabricates a call edge that never happened.
 */
data class MethodCounts(
    val name: String,
    val fileName: String,
    val calls: Long,
    val inclusiveNs: Long,
    val outsideSelectionCalls: Long,
)

class ProfileViewModel(
    val trace: ProfileTrace,
    val tracks: List<ProfileTrackView>,
    val totals: List<FlameTotal>,
    val launchSites: LaunchSiteIndex,
    val counts: Map<String, MethodCounts>,
    val startNs: Long,
    val endNs: Long,
) {
    val spanNs: Long get() = (endNs - startNs).coerceAtLeast(0)

    /** Tracks with program work on them, in writer order. */
    val workTracks: List<ProfileTrackView> get() = tracks.filter { it.kind.carriesWork }

    val hasDeviceWork: Boolean get() = tracks.any { it.kind == TrackKind.DEVICE_QUEUE }

    /** §8.5 — the call-count column appears only when there is data behind it. */
    val hasInstrumentation: Boolean get() = counts.isNotEmpty()

    /** The trace was cut short; the last packet was half-written. */
    val truncated: Boolean get() = trace.truncated

    /**
     * How this frame was measured, or null when it is not a device measurement.
     * See [MeasurementQuality.of] on why the absent annotation is refused rather
     * than defaulted.
     */
    fun qualityOf(node: FlameNode): MeasurementQuality? =
        MeasurementQuality.of(node.annotations)

    /** The call site that launched this kernel (§8.4), or null. */
    fun launchLocationOf(node: FlameNode): ProfileSourceLocation? =
        launchSites.locationFor(node)

    /**
     * Exact counts for a frame, when an instrumented build recorded them.
     *
     * The two halves name methods differently — the sampler writes
     * `Type.method` and the instrumentation writer writes `Type.method(File)` —
     * so the lookup is by the normalized name rather than by string equality,
     * which would silently return nothing for every method in the trace.
     */
    fun countsFor(node: FlameNode): MethodCounts? = counts[node.name]

    fun countsFor(total: FlameTotal): MethodCounts? = counts[total.name]

    /** Position of a timestamp on the shared axis, 0.0 .. 1.0. */
    fun fractionOf(ns: Long): Double {
        val span = spanNs
        if (span <= 0) return 0.0
        return ((ns - startNs).toDouble() / span.toDouble()).coerceIn(0.0, 1.0)
    }

    companion object {

        /** Strip the instrumentation writer's `(File)` suffix. */
        internal fun normalizeMethodName(raw: String): String {
            val paren = raw.indexOf('(')
            return if (paren > 0) raw.substring(0, paren) else raw
        }

        fun of(trace: ProfileTrace): ProfileViewModel {
            val flame = FlameGraph.build(trace)
            val flameByUuid = flame.associateBy { it.track.uuid }

            fun view(track: ProfileTrack): ProfileTrackView {
                val stamps = trace.events
                    .filter { it.trackUuid == track.uuid && it.timestampNs > 0 }
                    .map { it.timestampNs }
                return ProfileTrackView(
                    track = track,
                    kind = TrackKind.of(track.name),
                    roots = flameByUuid[track.uuid]?.roots ?: emptyList(),
                    startNs = stamps.minOrNull() ?: 0,
                    endNs = stamps.maxOrNull() ?: 0,
                )
            }

            // Every DECLARED track gets a view, slices or not — a device or
            // context descriptor carries the hierarchy its queues indent under,
            // and building views from the flame forest alone dropped exactly
            // those rows. Tracks that carry events without a descriptor (a
            // truncated trace) follow, under their synthesized names.
            val declared = trace.tracks.map { view(it) }
            val declaredUuids = trace.tracks.map { it.uuid }.toSet()
            val undeclared = flame
                .filter { it.track.uuid !in declaredUuids }
                .map { view(it.track) }
            val views = declared + undeclared

            // The axis spans tracks that carry work. Structure and metadata
            // tracks are excluded deliberately: the profiler's own run record
            // is stamped 0 (it summarizes the run rather than happening at a
            // moment), and letting it set the origin would compress every real
            // track into the right-hand edge of the view.
            val work = views.filter { it.kind.carriesWork && it.startNs > 0 }
            val start = work.minOfOrNull { it.startNs } ?: 0
            val end = work.maxOfOrNull { it.endNs } ?: start

            return ProfileViewModel(
                trace = trace,
                tracks = views,
                totals = FlameGraph.byName(flame),
                launchSites = KernelLaunchSites.of(trace),
                counts = readCounts(trace),
                startNs = start,
                endNs = end,
            )
        }

        private fun readCounts(trace: ProfileTrace): Map<String, MethodCounts> {
            val out = LinkedHashMap<String, MethodCounts>()
            val instr = trace.tracks
                .filter { TrackKind.of(it.name) == TrackKind.INSTRUMENTATION }
                .map { it.uuid }.toSet()
            if (instr.isEmpty()) return out

            for (e in trace.events) {
                if (e.trackUuid !in instr) continue
                val raw = e.name ?: continue
                val calls = e.annotations["calls"]?.toLongOrNull() ?: continue
                val name = normalizeMethodName(raw)
                out[name] = MethodCounts(
                    name = name,
                    fileName = e.sourceLocation?.fileName
                        ?: raw.substringAfter('(', "").substringBeforeLast(')'),
                    calls = calls,
                    inclusiveNs = e.annotations["inclusive_ns"]?.toLongOrNull() ?: 0,
                    outsideSelectionCalls =
                        e.annotations["outside_selection_calls"]?.toLongOrNull() ?: 0,
                )
            }
            return out
        }
    }
}
