# http-connectionframer-segv-connection-burst — the h1 server SIGSEGVs in `ConnectionFramer` under a burst of fresh connections (defect spec)

> Status: **draft**, filed 2026-10-25 from the primavera and http benches on
> cajeta-http 0.9.1, cajeta 0.46.3.

## 1. Definition
A freshly started server meeting 512 concurrent HTTP/1.1 connections at
once (oha, `-c 512`, the primavera `register-user-json` row) took a SIGSEGV
in cajeta-http's `ConnectionFramer` and then hung instead of exiting; the
bench's 600 s phase timeout turned it into a failed row and stopped the
process by PID. Unit 18 saw one intermittent SIGSEGV of the h1 server under
`-c 64` in the same class early on, not reproduced in six repeats. Distinct
from `http-h2c-crash-above-256-streams` (that one is the h2 dispatch pool).
The same session measured primavera's own throughput at about half the
earlier smoke (1.9k against 4.0k req/s) with the server lock unchanged and
the competitors faster; unexplained, possibly the busy box.

## 2. Use cases
- **2.1** As a server, when 512 connections arrive in the same instant on a
  cold process, then every one is accepted or refused cleanly and nothing
  crashes or hangs.
- **2.2** As the test suite, then a test opens 1,024 connections to a
  fresh server as fast as the client can and asserts the server answers all
  of them and exits cleanly on shutdown; the test runs under the address
  sanitizer once the memory-tooling spec lands.

## 3. Acceptance
`primavera/register-user-json` at 512 connections and `http/plaintext-h1` at
512 run ten times without a server crash.
