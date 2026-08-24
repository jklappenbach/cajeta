package dev.cajeta.idea.profiler

/**
 * cajeta-profiler 11.2.d — the track tree (spec §8.3).
 *
 * Tracks are not a flat list. A GPU queue belongs to a context, which belongs
 * to a device; a fiber belongs to the thread carrying it. The writer records
 * that with `parent_uuid`, and the timeline draws it as indentation, so a
 * reader can see that four queues are four lanes of ONE device rather than four
 * devices.
 */
data class TrackNode(
    val view: ProfileTrackView,
    val children: List<TrackNode>,
    val depth: Int,
) {
    /** This node and everything under it, in draw order. */
    fun flatten(): List<TrackNode> = listOf(this) + children.flatMap { it.flatten() }
}

object TrackHierarchy {

    /**
     * Build the forest.
     *
     * Two cases have to survive, and both occur in real traces:
     *
     * A track whose parent uuid names a track **not in the file** is treated as
     * a root rather than dropped. Track descriptors and events are written as
     * they are discovered, and a trace truncated mid-write can hold a queue
     * whose device descriptor never landed. Dropping it would hide the work.
     *
     * A **cycle** — which no correct writer emits, and which a corrupt file can
     * still contain — must not hang the UI. Parents are only ever taken from
     * tracks already placed, so a cycle degrades to roots instead of looping.
     */
    fun of(tracks: List<ProfileTrackView>): List<TrackNode> {
        val byUuid = tracks.associateBy { it.track.uuid }
        val childrenOf = HashMap<Long, MutableList<ProfileTrackView>>()
        val roots = ArrayList<ProfileTrackView>()

        for (t in tracks) {
            val parent = t.track.parentUuid
            if (parent != 0L && parent != t.track.uuid && byUuid.containsKey(parent)) {
                childrenOf.getOrPut(parent) { ArrayList() }.add(t)
            } else {
                roots.add(t)
            }
        }

        // Depth is bounded so a cycle among tracks that all claim each other as
        // parent cannot recurse without end.
        fun build(t: ProfileTrackView, depth: Int, seen: Set<Long>): TrackNode {
            val kids = if (depth >= MAX_DEPTH) emptyList()
            else (childrenOf[t.track.uuid] ?: emptyList())
                .filter { it.track.uuid !in seen }
                .map { build(it, depth + 1, seen + t.track.uuid) }
            return TrackNode(t, kids, depth)
        }

        return roots.map { build(it, 0, setOf(it.track.uuid)) }
    }

    /** Every track, parents before children, as the timeline stacks them. */
    fun flatten(tracks: List<ProfileTrackView>): List<TrackNode> =
        of(tracks).flatMap { it.flatten() }

    private const val MAX_DEPTH = 16
}
