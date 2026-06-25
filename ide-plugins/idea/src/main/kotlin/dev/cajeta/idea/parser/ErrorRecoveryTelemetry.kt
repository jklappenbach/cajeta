package dev.cajeta.idea.parser

import org.antlr.v4.runtime.Vocabulary
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicLong

/**
 * Aggregates how often [CajetaErrorStrategy] recovers and which token each
 * recovery lands on (W4b). The point is tuning: the strategy syncs to a curated
 * anchor set, and this tells us whether that set is right as the grammar
 * evolves — a recovery that lands on a NON-anchor token (one only in ANTLR's
 * default follow set) is a hint the anchor set should grow, and a token that
 * dominates the counts is a hint about where recoveries cluster.
 *
 * Process-global, in-memory, thread-safe, and intentionally lossy — this is
 * telemetry, not correctness state. Cheap enough to leave always-on.
 */
object ErrorRecoveryTelemetry {
    private val byToken = ConcurrentHashMap<Int, AtomicLong>()
    private val anchorLandings = AtomicLong()
    private val nonAnchorLandings = AtomicLong()

    /** Record one recovery that landed on [tokenType]; [onAnchor] = it was one
     *  of CajetaErrorStrategy's curated anchors (vs a default-set token). */
    fun record(tokenType: Int, onAnchor: Boolean) {
        byToken.computeIfAbsent(tokenType) { AtomicLong() }.incrementAndGet()
        (if (onAnchor) anchorLandings else nonAnchorLandings).incrementAndGet()
    }

    fun total(): Long = anchorLandings.get() + nonAnchorLandings.get()
    fun anchorLandings(): Long = anchorLandings.get()
    fun nonAnchorLandings(): Long = nonAnchorLandings.get()
    fun countFor(tokenType: Int): Long = byToken[tokenType]?.get() ?: 0L

    /** Per-token tallies, busiest first; [vocab] (e.g. `CajetaLexer.VOCABULARY`)
     *  resolves symbolic names for human inspection. */
    fun snapshot(vocab: Vocabulary? = null): List<Entry> =
        byToken.entries
            .map { Entry(it.key, vocab?.getSymbolicName(it.key), it.value.get()) }
            .sortedByDescending { it.count }

    fun reset() {
        byToken.clear()
        anchorLandings.set(0)
        nonAnchorLandings.set(0)
    }

    data class Entry(val tokenType: Int, val tokenName: String?, val count: Long)
}
