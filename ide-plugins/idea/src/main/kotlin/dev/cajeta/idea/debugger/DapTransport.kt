package dev.cajeta.idea.debugger

import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.nio.charset.StandardCharsets

/**
 * Debug Adapter Protocol message framing over a byte stream: each message is
 * a `Content-Length: N\r\n\r\n` header block followed by exactly N bytes of
 * UTF-8 JSON. This is the same framing `cajeta dap`
 * (`src/cajeta/dap/DapProtocol.cpp`) reads and writes.
 *
 * Reads are header-byte-precise: headers are consumed a byte at a time so the
 * reader never over-reads into the body of the next message. Writes are
 * serialized under a lock so concurrent senders can't interleave a header
 * with another message's body.
 */
class DapTransport(
    private val input: InputStream,
    private val output: OutputStream,
) {
    private val writeLock = Any()

    /** Frame and send one message. */
    fun write(message: Json) {
        val body = message.toCompactString().toByteArray(StandardCharsets.UTF_8)
        val header = "Content-Length: ${body.size}\r\n\r\n"
            .toByteArray(StandardCharsets.US_ASCII)
        synchronized(writeLock) {
            output.write(header)
            output.write(body)
            output.flush()
        }
    }

    /**
     * Read one framed message. Returns null at a clean end-of-stream (no bytes
     * available where a header would start). Throws [IOException] on a
     * truncated or malformed frame.
     */
    fun read(): Json? {
        val contentLength = readContentLength() ?: return null
        val body = ByteArray(contentLength)
        var off = 0
        while (off < contentLength) {
            val n = input.read(body, off, contentLength - off)
            if (n < 0) throw IOException("EOF after $off/$contentLength body bytes")
            off += n
        }
        return Json.parse(String(body, StandardCharsets.UTF_8))
    }

    /**
     * Consume the header block and return the Content-Length value. Returns
     * null if the stream ends cleanly before any header byte arrives.
     */
    private fun readContentLength(): Int? {
        var contentLength: Int? = null
        var sawAnyByte = false
        while (true) {
            val line = readLine(firstLine = !sawAnyByte) ?: return null
            sawAnyByte = true
            if (line.isEmpty()) break // blank line terminates the header block
            val colon = line.indexOf(':')
            if (colon < 0) continue
            val name = line.substring(0, colon).trim()
            if (name.equals("Content-Length", ignoreCase = true)) {
                contentLength = line.substring(colon + 1).trim().toIntOrNull()
                    ?: throw IOException("invalid Content-Length: '$line'")
            }
        }
        return contentLength
            ?: throw IOException("header block missing Content-Length")
    }

    /**
     * Read one CRLF- (or LF-) terminated header line as ASCII. Returns null on
     * a clean EOF before the line's first byte (only meaningful when
     * [firstLine] is true); throws on EOF mid-line.
     */
    private fun readLine(firstLine: Boolean): String? {
        val buf = ByteArrayOutputStream(32)
        var read = 0
        while (true) {
            val b = input.read()
            if (b < 0) {
                if (read == 0 && firstLine) return null
                throw IOException("EOF inside header line")
            }
            read++
            if (b == '\n'.code) break
            if (b != '\r'.code) buf.write(b)
        }
        return buf.toString(StandardCharsets.US_ASCII)
    }
}
