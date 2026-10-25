# http-h2c-crash-above-256-streams — the HTTP/2 server crashes or stalls past `maxInFlight` (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 18
> (cajeta-http bench) against cajeta-http 0.9.1 on cajeta 0.46.3. Reproduces
> standalone.

## 1. Definition
The default `maxInFlight` is 256. Under `oha --http2 -c 64 -p 8` (512
concurrent streams) the h2c server SIGSEGVs or stalls; 256 streams
(`-c 64 -p 4`, `-c 32 -p 8`) run clean. The crash is in
`Channel<Dispatch>.trySend` → `Dispatch` drop, reached from
`DispatchPool.offerOwned` ← `ConnectionFramer.offerStreams` ← `retryWaiting`:
an owned dispatch is dropped on the rejected-send path while still referenced.
The bench's `plaintext-h2` passed once at 2048 streams cut off at the deadline;
`json-h2` fails. An intermittent SIGSEGV of the h1 server under `-c 64`
(in `ConnectionFramer`) was seen once early and did not recur in six repeats.

## 2. Use cases
- **2.1** As a server, when more streams arrive than `maxInFlight`, then the
  excess wait or are refused with RST_STREAM(REFUSED_STREAM); nothing is
  freed twice and nothing crashes.
- **2.2** As the test suite, then a test opens 2 × `maxInFlight` streams over
  one connection and asserts every request completes or is refused cleanly.

## 3. Acceptance
`http/plaintext-h2` and `http/json-h2` pass the bench smoke at `-c 64 -p 8`
with zero errors.
