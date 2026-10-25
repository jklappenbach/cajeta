# udp-recvfromasync-parks-every-receive — `UdpSocket.recvFromAsync` parks on the reactor before every receive (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 10 on
> v0.46.3, measured on Phoenix (WSL2).

## 1. Definition
`UdpSocket.recvFromAsync` parks the fiber on the reactor before each receive
even when a datagram is already queued: 141 µs per datagram. `TcpStream.readAsync`
tries the read first and parks only on would-block. A receiver written to
read non-blocking and park only when the socket would block measures 9 µs
per datagram; the benchmark does that and says so. From the same pass, the
reactor's TCP echo round trip on this box is about 474 µs against 17 µs for
Go and 110 µs for threaded C++, Rust and Java, and server echo throughput
about 38k messages per 2 s against tokio's 334k and Go's 935k: smoke numbers,
but the ratio is the standing this page will carry until the wake path is
looked at.

## 2. Use cases
- **2.1** As a UDP receiver, when datagrams are queued, then `recvFromAsync`
  returns them without a reactor round trip, parking only on would-block,
  as `readAsync` does.
- **2.2** As the test suite, then a test sends 1e4 datagrams to a bound
  socket and asserts the receive loop completes within a bound an order of
  magnitude under the parked path.

## 3. Acceptance
`io/udp-send-rate` uses `recvFromAsync` directly and its note about the
workaround is removed; `io/tcp-echo-latency`'s standing names the reactor
wake cost if it remains.
