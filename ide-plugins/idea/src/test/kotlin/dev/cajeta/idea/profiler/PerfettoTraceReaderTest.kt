package dev.cajeta.idea.profiler

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * cajeta-profiler 11.1.a — a `.pftrace` written by Unit 6 parses in Kotlin.
 *
 * The fixture is a REAL trace: `samples/tour` run under `CAJETA_PROFILER=1`,
 * 220 packets over 6 tracks. A synthetic fixture built by this test would only
 * prove the reader agrees with whatever the test author believed the writer
 * does, and the two beliefs would drift together.
 *
 * Every expected value below was taken from Perfetto's own `trace_processor`
 * reading the same file, not from this reader. That is the point: the viewer
 * and the reference implementation have to agree about what the trace says,
 * because §8.8 promises a developer can open the same file in either.
 *
 *   trace_processor v57.2, tour.pftrace, 2026-08-23:
 *     tracks                6
 *     slices               58
 *     distinct slice names 48
 *     min(ts)   251789430045290
 *     max(ts)   251789494878285
 */
class PerfettoTraceReaderTest {

    private fun fixture(): ByteArray {
        val url = javaClass.classLoader.getResource("profiler/tour.pftrace")
        assertNotNull("fixture profiler/tour.pftrace is missing", url)
        return File(url!!.toURI()).readBytes()
    }

    private fun trace(): ProfileTrace = PerfettoTraceReader.read(fixture())

    @Test
    fun theFixtureParses() {
        val t = trace()
        assertEquals("packet count disagrees with the file", 220, t.packetCount)
    }

    @Test
    fun everyTrackIsFoundWithItsName() {
        val t = trace()
        assertEquals(6, t.tracks.size)
        // trace_processor reports these six names for this file.
        assertEquals(
            listOf(
                "cajeta.profiler",
                "cajeta.thread.0",
                "cajeta.fiber.1",
                "cajeta.fiber.2",
                "cajeta.fiber.24",
                "cajeta.fiber.25",
            ).sorted(),
            t.tracks.map { it.name }.sorted(),
        )
        // uuids have to be distinct or events cannot be attributed at all.
        assertEquals(6, t.tracks.map { it.uuid }.distinct().size)
    }

    @Test
    fun sliceCountMatchesTheReferenceImplementation() {
        val t = trace()
        // trace_processor collapses BEGIN/END pairs into 58 slices. A named
        // event is a BEGIN or an INSTANT; an END carries no name. Counting
        // named events is therefore the same 58 by a different route, and
        // disagreeing would mean one of us is losing events.
        assertEquals(58, t.events.count { it.name != null })
        assertEquals(48, t.events.mapNotNull { it.name }.distinct().size)
    }

    @Test
    fun timestampsSpanTheRangeTheReferenceReports() {
        val t = trace()
        val stamped = t.events.filter { it.timestampNs > 0 }
        assertTrue(stamped.isNotEmpty())
        assertEquals(251789430045290L, stamped.minOf { it.timestampNs })
        assertEquals(251789494878285L, stamped.maxOf { it.timestampNs })
    }

    @Test
    fun internedNamesAreResolvedNotLeftAsIds() {
        val t = trace()
        // The writer interns event names: 57 of the 58 named events carry only
        // a name_iid into InternedData.event_names. A reader that skipped
        // interning would produce a trace of anonymous slices that still parsed
        // — which is why this asserts a name the tour actually produces.
        val names = t.events.mapNotNull { it.name }.toSet()
        assertTrue("interned names were not resolved: $names",
                   names.contains("tour.Tour.main"))
        assertTrue(names.contains("cajeta.lang.stream.Stream<tour.DemoClass>.forEach"))
    }

    @Test
    fun theFirstEventsAreTheOnesTheReferenceOrdersFirst() {
        val t = trace()
        val firstNames = t.events.filter { it.name != null }
            .sortedBy { it.timestampNs }
            .take(4)
            .map { it.name }
        // trace_processor, ordered by ts: cajeta.profiler.run, tour.Tour.main,
        // ...Stream<tour.DemoClass>.forEach, tour.Tour.<lambda>. All four share
        // one timestamp, so this asserts membership rather than an order the
        // reference does not actually fix.
        assertTrue(
            "first events disagree with trace_processor: $firstNames",
            firstNames.containsAll(listOf("cajeta.profiler.run", "tour.Tour.main")),
        )
    }

    @Test
    fun sourceLocationsAreResolvedSoAFrameCanBeNavigatedTo() {
        val t = trace()
        // §8.2 — selecting a frame navigates to a source location. That is only
        // possible if the interned SourceLocation table is read; 52 entries in
        // this file.
        val located = t.events.filter { it.sourceLocation != null }
        assertTrue("no event resolved a source location", located.isNotEmpty())
        val loc = located.first().sourceLocation!!
        assertTrue("a source location with no file name cannot be navigated to",
                   loc.fileName.isNotEmpty())
    }

    @Test
    fun runMetadataAnnotationsSurvive() {
        val t = trace()
        // §7.8's run metadata rides as debug annotations. trace_processor sees
        // these as debug.tier, debug.rate_hz, debug.samples_taken, ... — a
        // reader that dropped them would render a trace with no way to tell
        // how it was captured.
        val keys = t.events.flatMap { it.annotations.keys }.toSet()
        assertTrue("run metadata is missing: $keys", keys.contains("tier"))
        assertTrue(keys.contains("rate_hz"))
        assertTrue(keys.contains("samples_taken"))
    }

    @Test
    fun anEmptyFileParsesToAnEmptyTraceRatherThanThrowing() {
        // Opening a trace whose run has not written anything yet is ordinary.
        val t = PerfettoTraceReader.read(ByteArray(0))
        assertEquals(0, t.packetCount)
        assertTrue(t.tracks.isEmpty())
        assertTrue(t.events.isEmpty())
    }

    @Test
    fun aTruncatedTraceYieldsThePacketsItDidContain() {
        // A profiled process can be killed mid-write. The packets already on
        // disk are still a valid measurement, and losing all of them because
        // the last one is half-written would be the worst possible response.
        val full = fixture()
        val cut = full.copyOf(full.size / 2)
        val t = PerfettoTraceReader.read(cut)
        assertTrue("a truncated trace yielded nothing at all", t.packetCount > 0)
        assertTrue(t.packetCount < 220)
        assertTrue("tracks are declared early and should survive truncation",
                   t.tracks.isNotEmpty())
    }
}
