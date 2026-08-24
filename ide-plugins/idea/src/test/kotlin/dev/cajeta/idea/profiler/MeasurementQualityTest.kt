package dev.cajeta.idea.profiler

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * cajeta-profiler 11.1.f — degraded-tier and low-confidence measurements are
 * visually distinct (spec §8.6, §10.6, §11.3).
 *
 * The runtime is careful to say how it measured each span: the tier that
 * produced it, the confidence of the clock correlation behind it, and any
 * integrity flag the checker raised. §10.6 puts those on the MEASUREMENT rather
 * than on the run, because a single run mixes them — one backend demoted,
 * another not — and a developer must never have to infer that the span in front
 * of them was degraded.
 *
 * All of that care is wasted if the viewer renders every span identically. The
 * model here is what the UI keys off, so the distinction is testable without a
 * screenshot.
 *
 * ## The absent annotation must not read as the strongest claim
 *
 * `CAJETA_PROF_TIER_DEVICE` is **0**, so the zero value is the most confident
 * thing a span can say. A sampled CPU frame carries no tier annotation at all,
 * and a reader that defaults a missing field to 0 promotes every host frame in
 * the trace to "measured on the device". The same hazard has already produced a
 * real trace on this machine — 144 spans claiming device tier behind a backend
 * that reported zero dispatch records (plan 6.7) — so the viewer refuses the
 * default rather than inheriting it.
 */
class MeasurementQualityTest {

    private fun trace(): ProfileTrace {
        val url = javaClass.classLoader.getResource("profiler/gpu.pftrace")
        assertNotNull("fixture profiler/gpu.pftrace is missing", url)
        return PerfettoTraceReader.read(File(url!!.toURI()).readBytes())
    }

    private fun tourTrace(): ProfileTrace {
        val url = javaClass.classLoader.getResource("profiler/tour.pftrace")
        assertNotNull("fixture profiler/tour.pftrace is missing", url)
        return PerfettoTraceReader.read(File(url!!.toURI()).readBytes())
    }

    private fun deviceSpans(t: ProfileTrace): List<ProfileEvent> {
        val queues = t.tracks.filter { it.name.startsWith("queue ") }.map { it.uuid }.toSet()
        return t.events.filter { it.trackUuid in queues && it.isBegin }
    }

    private fun qualities() = deviceSpans(trace()).map { MeasurementQuality.of(it)!! }

    // --- the tiers are told apart --------------------------------------------

    @Test
    fun theFixtureCarriesAllThreeTiers() {
        // If it did not, every assertion below about telling them apart would
        // be passing on absent data.
        val tiers = qualities().map { it.tier }.toSet()
        assertEquals(setOf(ProfileTier.DEVICE, ProfileTier.EVENT, ProfileTier.HOST), tiers)
    }

    @Test
    fun aDeviceTierSpanIsNotDegraded() {
        val q = qualities().first { it.tier == ProfileTier.DEVICE && it.integrityFlags == 0 }
        assertFalse("a vendor dispatch record is the best measurement available", q.degraded)
        assertFalse(q.flagged)
    }

    @Test
    fun hostAndEventTiersAreBothDegradedAndStillDistinguishable() {
        val host = qualities().first { it.tier == ProfileTier.HOST }
        val event = qualities().first { it.tier == ProfileTier.EVENT }
        assertTrue("host submit-to-complete is a degraded measurement", host.degraded)
        assertTrue("event bracketing is a degraded measurement", event.degraded)
        // §10.4's ladder has rungs. Collapsing them to one "degraded" bucket
        // would hide that a backend fell one step rather than all the way.
        assertTrue("the two degraded tiers render identically", host.tier != event.tier)
        assertTrue(host.tier.ordinal > event.tier.ordinal)
    }

    // --- integrity ------------------------------------------------------------

    @Test
    fun aFlaggedSpanIsNotTrustedEvenAtDeviceTier() {
        val q = qualities().first { it.integrityFlags != 0 }
        assertEquals(ProfileTier.DEVICE, q.tier)
        assertTrue("a span the integrity checker flagged was reported as trusted", q.flagged)
        assertFalse("tier alone decided trust, ignoring §11.3's flags", q.trusted)
    }

    @Test
    fun aFlagIsNamedRatherThanShownAsANumber() {
        val q = qualities().first { it.integrityFlags != 0 }
        assertEquals(SpanIntegrity.OUTSIDE_HOST, q.integrityFlags)
        assertTrue("no reason given for a flagged span", q.reasons.isNotEmpty())
        assertTrue("reasons ${q.reasons} do not mention the launch window",
            q.reasons.any { it.contains("launch", ignoreCase = true) })
    }

    @Test
    fun everyFlagBitIsDecodedSeparately() {
        val both = SpanIntegrity.NONMONOTONIC or SpanIntegrity.OUTSIDE_HOST
        assertEquals(2, SpanIntegrity.describe(both).size)
        assertEquals(0, SpanIntegrity.describe(SpanIntegrity.OK).size)
        assertEquals(5, SpanIntegrity.describe(0b11111).size)
    }

    @Test
    fun anUnknownFlagBitStillMarksTheSpan() {
        // A trace from a newer runtime may raise a flag this build has never
        // heard of. Rendering it as clean would be the worst reading of it.
        val future = 1 shl 20
        val q = MeasurementQuality(
            tier = ProfileTier.DEVICE, clockConfidence = 100, integrityFlags = future)
        assertTrue("an unrecognised integrity flag was treated as clean", q.flagged)
        assertFalse(q.trusted)
        assertTrue("no reason given for an unrecognised flag", q.reasons.isNotEmpty())
    }

    // --- clock confidence ------------------------------------------------------

    @Test
    fun zeroConfidenceIsTreatedAsNoTrustworthyCorrelation() {
        // §11.6: 0 means the profiler could not establish a trustworthy
        // correlation, and every consumer must treat it as "do not render this
        // as a timeline". It is not merely a low score.
        val q = MeasurementQuality(ProfileTier.DEVICE, clockConfidence = 0, integrityFlags = 0)
        assertFalse(q.trusted)
        assertTrue(q.uncorrelated)
        assertTrue(q.reasons.any { it.contains("correlation", ignoreCase = true) })
    }

    @Test
    fun aConvergedFitIsTrustedAndAWeakOneIsMarkedLowConfidence() {
        val strong = MeasurementQuality(ProfileTier.DEVICE, 100, 0)
        assertTrue(strong.trusted)
        assertFalse(strong.lowConfidence)

        // The runtime caps a single-sample fit at 40 no matter how tight it
        // was, because one sample cannot see rate error at all.
        val weak = MeasurementQuality(ProfileTier.DEVICE, 40, 0)
        assertTrue("a single-sample fit was rendered as a converged one", weak.lowConfidence)
        assertFalse(weak.uncorrelated)
        assertFalse(weak.trusted)
    }

    // --- the absent annotation -------------------------------------------------

    @Test
    fun aSpanWithNoTierAnnotationIsNotAMeasuredDeviceSpan() {
        // The 6.7 hazard, guarded on the viewer side: TIER_DEVICE is 0, so a
        // missing annotation defaulted to 0 would promote the frame.
        val plain = ProfileEvent(
            trackUuid = 1, type = PerfettoTraceReader.TYPE_SLICE_BEGIN,
            timestampNs = 100, name = "main", sourceLocation = null,
        )
        assertNull("a frame with no tier annotation was given a tier", MeasurementQuality.of(plain))
    }

    @Test
    fun noSampledCpuFrameInTheTourTraceClaimsDeviceTier() {
        // The whole tour fixture is CPU work. Not one frame may come back as a
        // device measurement.
        val t = tourTrace()
        val claimed = t.events.filter { it.isBegin }
            .mapNotNull { MeasurementQuality.of(it) }
        assertTrue(
            "${claimed.size} sampled CPU frames were reported as device-tier " +
                "measurements; TIER_DEVICE is 0 and the absent annotation was defaulted",
            claimed.isEmpty(),
        )
    }

    @Test
    fun anExplicitlyUnknownTierIsDegradedNotPromoted() {
        // A tier value this build does not recognise is a claim it cannot
        // verify, so it is ranked with the degraded ones rather than the best.
        val q = MeasurementQuality(ProfileTier.of(99), 100, 0)
        assertEquals(ProfileTier.UNKNOWN, q.tier)
        assertTrue(q.degraded)
        assertFalse(q.trusted)
    }

    // --- the distinction is actually available to a renderer --------------------

    @Test
    fun theFixtureProducesMoreThanOneRenderingClass() {
        // "Visually distinct" is vacuous if every span in a real trace lands in
        // the same bucket.
        val classes = qualities().map { it.renderClass }.toSet()
        assertTrue("every span renders identically: $classes", classes.size > 1)
    }

    @Test
    fun everyUntrustedSpanCanSayWhy() {
        for (q in qualities()) {
            if (q.trusted) continue
            assertTrue("an untrusted span offered no reason (tier=${q.tier}, " +
                "conf=${q.clockConfidence}, flags=${q.integrityFlags})",
                q.reasons.isNotEmpty())
        }
    }
}
