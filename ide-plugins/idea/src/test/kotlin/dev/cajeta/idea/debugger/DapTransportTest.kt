package dev.cajeta.idea.debugger

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.nio.charset.StandardCharsets

/** Unit tests for DAP Content-Length framing. */
class DapTransportTest {

    private fun frameBytes(json: String): ByteArray {
        val body = json.toByteArray(StandardCharsets.UTF_8)
        val header = "Content-Length: ${body.size}\r\n\r\n".toByteArray(StandardCharsets.US_ASCII)
        return header + body
    }

    @Test
    fun writeEmitsContentLengthHeaderThenBody() {
        val out = ByteArrayOutputStream()
        val transport = DapTransport(InputStream.nullInputStream(), out)
        transport.write(Json.obj("command" to Json.of("initialize")))

        val raw = out.toByteArray().toString(StandardCharsets.UTF_8)
        val sep = raw.indexOf("\r\n\r\n")
        assertTrue("missing header separator", sep > 0)
        val header = raw.substring(0, sep)
        val body = raw.substring(sep + 4)
        assertEquals("""{"command":"initialize"}""", body)
        assertEquals(
            "Content-Length: ${body.toByteArray(StandardCharsets.UTF_8).size}",
            header,
        )
    }

    @Test
    fun readParsesASingleFrame() {
        val input = ByteArrayInputStream(frameBytes("""{"seq":1,"type":"event","event":"stopped"}"""))
        val transport = DapTransport(input, OutputStream.nullOutputStream())
        val msg = transport.read()!!
        assertEquals("event", msg.at("type").asString())
        assertEquals("stopped", msg.at("event").asString())
    }

    @Test
    fun readConsumesBackToBackFramesWithoutOverreading() {
        val stream = frameBytes("""{"seq":1}""") + frameBytes("""{"seq":2}""")
        val transport = DapTransport(ByteArrayInputStream(stream), OutputStream.nullOutputStream())
        assertEquals(1, transport.read()!!.at("seq").asInt())
        assertEquals(2, transport.read()!!.at("seq").asInt())
        assertNull("third read past EOF should be null", transport.read())
    }

    @Test
    fun readReturnsNullOnCleanEof() {
        val transport = DapTransport(InputStream.nullInputStream(), OutputStream.nullOutputStream())
        assertNull(transport.read())
    }

    @Test
    fun writeThenReadRoundTripsOverAPipe() {
        // Serialize with the writer, then feed those exact bytes to the reader.
        val out = ByteArrayOutputStream()
        DapTransport(InputStream.nullInputStream(), out).write(
            Json.obj(
                "type" to Json.of("response"),
                "request_seq" to Json.of(42),
                "body" to Json.obj("value" to Json.of("<Type@0xabc>")),
            ),
        )
        val back = DapTransport(ByteArrayInputStream(out.toByteArray()), OutputStream.nullOutputStream()).read()!!
        assertEquals(42, back.at("request_seq").asInt())
        assertEquals("<Type@0xabc>", back.at("body").at("value").asString())
    }
}
