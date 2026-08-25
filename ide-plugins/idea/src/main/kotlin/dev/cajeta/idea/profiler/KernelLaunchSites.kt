package dev.cajeta.idea.profiler

import com.intellij.openapi.project.Project

/**
 * cajeta-profiler 11.1.e — from a kernel back to the line that launched it
 * (spec §8.4).
 *
 * A kernel runs on a device queue. The code that launched it ran earlier, on a
 * host thread, on a different track — and by the time the kernel executes, that
 * call has usually already returned. The only thing connecting them is the flow
 * the writer emits: the host launch site lists the id, the device slice
 * terminates it, and the id is the launch id the seam minted.
 *
 * ## The link is the flow, and only the flow
 *
 * Two substitutes suggest themselves and both are wrong in ways that do not
 * announce themselves:
 *
 * **By name** collapses every launch of the same kernel onto one call site. A
 * kernel launched from three places in a loop nest reports whichever the lookup
 * happened to see first.
 *
 * **By time** — the device slice nearest the launch — fails under precisely the
 * conditions a profiler exists to reveal. Dispatch is asynchronous, so the next
 * device slice to start is frequently not the one the last launch produced, and
 * with overlapping streams there is no "nearest" to speak of.
 *
 * Either would send a developer to a real line of their own code with nothing
 * to suggest it was the wrong one. That is the §8.2 failure a basename fallback
 * would have caused, one level up: opening nothing is recoverable, opening the
 * wrong thing is not. So an unlinked kernel resolves to null.
 */
class LaunchSiteIndex internal constructor(
    private val byFlow: Map<Long, ProfileEvent>,
) {
    /** How many launch sites the trace carried. */
    val size: Int get() = byFlow.size

    fun siteFor(flowId: Long): ProfileEvent? = byFlow[flowId]

    /**
     * The launch site behind a device slice.
     *
     * A slice terminating more than one flow is not something this writer
     * emits; if one ever arrives, the first that resolves is used rather than
     * the call failing — a kernel with two candidate launches is still better
     * placed than a kernel with none.
     */
    fun siteFor(event: ProfileEvent): ProfileEvent? =
        event.terminatingFlowIds.firstNotNullOfOrNull { byFlow[it] }

    fun siteFor(node: FlameNode): ProfileEvent? =
        node.terminatingFlowIds.firstNotNullOfOrNull { byFlow[it] }

    /** Where the launching call was written. Null when the flow leads nowhere. */
    fun locationFor(event: ProfileEvent): ProfileSourceLocation? =
        siteFor(event)?.sourceLocation

    fun locationFor(node: FlameNode): ProfileSourceLocation? =
        siteFor(node)?.sourceLocation

    /**
     * Open the launching call site of a kernel. Returns false when the kernel
     * carries no flow, when the flow leads to a site with no recorded location,
     * or when that location does not resolve — the three ways §8.4 can have
     * nothing to show, none of which is worth guessing through.
     */
    fun open(project: Project, node: FlameNode): Boolean {
        val loc = locationFor(node) ?: return false
        return ProfileNavigation.open(project, loc)
    }
}

object KernelLaunchSites {

    /**
     * Index the launch sites in a trace.
     *
     * Built from events that START a flow. Those are instants on a host thread
     * track, written beside the dispatch; nothing else in a cajeta trace emits
     * `flow_ids`, so no filter by track kind is needed and adding one would
     * silently drop launches from a track this build has not heard of.
     */
    fun of(trace: ProfileTrace): LaunchSiteIndex {
        val byFlow = HashMap<Long, ProfileEvent>()
        for (e in trace.events) {
            for (id in e.flowIds) {
                // First writer wins. Ids come from a monotonic counter, so a
                // repeat means a trace that wrapped or was concatenated, and
                // the earlier site is the one the earlier kernel belongs to.
                byFlow.putIfAbsent(id, e)
            }
        }
        return LaunchSiteIndex(byFlow)
    }
}
