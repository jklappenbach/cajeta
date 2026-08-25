package dev.cajeta.idea.profiler

/**
 * cajeta-profiler 11.1.f — how a span was measured, and whether to believe it
 * (spec §8.6, §10.6, §11.3, §11.6).
 *
 * The runtime annotates each device span with the tier that produced it, the
 * confidence of the clock correlation behind it, and any integrity flag the
 * checker raised. §10.6 attaches these to the measurement rather than to the
 * run because one run mixes them — one backend demoted, another not.
 *
 * This is the model a renderer keys off, so "visually distinct" is testable
 * without a screenshot.
 */

/**
 * How a span's timing was obtained. Lower id is better among the real tiers —
 * and id 0 is deliberately NOT one of them: the runtime mints every event by
 * memset, so 0 is the value an event carries when nothing assigned its tier,
 * and it maps to [UNKNOWN] (the runtime renumbered it so on 2026-08-24, plan
 * 6.7.2.a — before that, 0 was DEVICE and the unassigned default read as the
 * strongest claim a span can make).
 */
enum class ProfileTier(val id: Int, val label: String) {
    /** A vendor profiler dispatch record: the device said so itself. */
    DEVICE(1, "device"),

    /** Device event bracketing — real device time, coarser. */
    EVENT(2, "event"),

    /** Host submit-to-complete. True, and about a wider thing than the kernel. */
    HOST(3, "host"),

    /**
     * A tier id this build does not recognise — including 0, the unassigned
     * default. Ranked LAST, never first: an unrecognised claim is one that
     * cannot be verified, and a newer runtime adding a tier must not have it
     * silently rendered as the best available.
     */
    UNKNOWN(-1, "unknown");

    /** Anything short of a dispatch record is a degraded measurement (§10.4). */
    val degraded: Boolean get() = this != DEVICE

    companion object {
        fun of(id: Int): ProfileTier = entries.firstOrNull { it.id == id && it != UNKNOWN } ?: UNKNOWN
    }
}

/**
 * §11.3's per-span integrity flags, as the runtime defines them in
 * `cajeta_prof_abi.h`. A flagged span still renders — dropping it would read as
 * idle time — and only the annotation says it should not be trusted.
 */
object SpanIntegrity {
    const val OK = 0
    const val NONMONOTONIC = 1 shl 0
    const val NEGATIVE = 1 shl 1
    const val IMPLAUSIBLE = 1 shl 2
    const val UNCORRELATED = 1 shl 3
    const val OUTSIDE_HOST = 1 shl 4

    private val NAMED = listOf(
        NONMONOTONIC to "started before the span before it",
        NEGATIVE to "ends before it starts",
        IMPLAUSIBLE to "duration outside any sane bound",
        UNCORRELATED to "no trustworthy clock mapping",
        OUTSIDE_HOST to "device span outside its launch-to-resolution bracket",
    )

    /** One human-readable reason per raised bit. */
    fun describe(flags: Int): List<String> {
        if (flags == OK) return emptyList()
        val out = NAMED.filter { (bit, _) -> flags and bit != 0 }.map { it.second }
        // A trace from a newer runtime can raise a bit this build has never
        // heard of. Reporting it as clean is the worst available reading, so
        // the leftovers are named as a group rather than dropped.
        val known = NAMED.fold(0) { acc, (bit, _) -> acc or bit }
        val unknown = flags and known.inv()
        return if (unknown != 0) out + "unrecognised integrity flags (0x${unknown.toString(16)})"
        else out
    }
}

/** How a renderer should distinguish this span. */
enum class RenderClass { TRUSTED, LOW_CONFIDENCE, DEGRADED, FLAGGED, UNCORRELATED }

data class MeasurementQuality(
    val tier: ProfileTier,
    /** 0 means no trustworthy correlation at all (§11.6); 10..100 otherwise. */
    val clockConfidence: Int,
    val integrityFlags: Int,
) {
    val degraded: Boolean get() = tier.degraded

    val flagged: Boolean get() = integrityFlags != SpanIntegrity.OK

    /**
     * §11.6 — zero is not a low score, it is the profiler saying it could not
     * establish a correlation. A consumer must not render it as a timeline.
     */
    val uncorrelated: Boolean get() = clockConfidence == 0

    /**
     * The runtime caps a one-sample fit at 40 however tight it was, because one
     * sample cannot observe rate error. Anything at or below that rests on too
     * little to be read as a converged fit.
     */
    val lowConfidence: Boolean get() = clockConfidence in 1..40

    val trusted: Boolean
        get() = !degraded && !flagged && !uncorrelated && !lowConfidence

    /**
     * Ordered worst-first: a span that is both flagged and degraded is rendered
     * as flagged, because that is the more urgent thing to say about it.
     */
    val renderClass: RenderClass
        get() = when {
            flagged -> RenderClass.FLAGGED
            uncorrelated -> RenderClass.UNCORRELATED
            degraded -> RenderClass.DEGRADED
            lowConfidence -> RenderClass.LOW_CONFIDENCE
            else -> RenderClass.TRUSTED
        }

    /** Everything wrong with this measurement, in words. Empty when trusted. */
    val reasons: List<String>
        get() = buildList {
            if (uncorrelated) add("no trustworthy clock correlation")
            else if (lowConfidence) add("clock correlation rests on a weak fit ($clockConfidence/100)")
            when (tier) {
                ProfileTier.HOST -> add("timed by host submit-to-complete, not by the device")
                ProfileTier.EVENT -> add("timed by device event bracketing, not by a dispatch record")
                ProfileTier.UNKNOWN -> add("measured by a tier this build does not recognise")
                ProfileTier.DEVICE -> {}
            }
            addAll(SpanIntegrity.describe(integrityFlags))
        }

    companion object {
        private const val TIER = "tier"
        private const val CONFIDENCE = "clock_confidence"
        private const val INTEGRITY = "integrity_flags"

        /**
         * The quality of a span, or **null** when the event is not a measured
         * device span at all.
         *
         * Null rather than a default is the whole of this function's care. A
         * sampled CPU frame carries no `tier` annotation at all — it is not a
         * device measurement, degraded or otherwise — and defaulting the
         * absent annotation to any tier would put every host frame in the
         * quality legend. (The runtime-side twin of this hazard — memsetting
         * an event whose 0-valued tier then read as DEVICE, the strongest
         * claim — was closed by plan 6.7.2.a's renumber: 0 is now UNKNOWN on
         * both sides.)
         */
        fun of(event: ProfileEvent): MeasurementQuality? = of(event.annotations)

        fun of(annotations: Map<String, String>): MeasurementQuality? {
            // toIntOrNull, not a presence check: the run-record instant also
            // carries a `tier` annotation and its value is a STRING
            // ("sampling", "instrumentation"). Treating that as a measured span
            // would put the profiler's own metadata in the quality legend.
            val tier = annotations[TIER]?.toIntOrNull() ?: return null
            return MeasurementQuality(
                tier = ProfileTier.of(tier),
                clockConfidence = annotations[CONFIDENCE]?.toIntOrNull() ?: 0,
                // Absent means OK here, and that IS safe: the writer omits the
                // annotation only when the checker returned CAJETA_SPAN_OK.
                integrityFlags = annotations[INTEGRITY]?.toIntOrNull() ?: SpanIntegrity.OK,
            )
        }
    }
}
