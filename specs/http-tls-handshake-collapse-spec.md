# http-tls-handshake-collapse — TLS handshake rate collapses with concurrency (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 18 against
> cajeta-http 0.9.1 with dev.cajeta.crypto, on cajeta 0.46.3.

## 1. Definition
`http/tls-handshake-rate` (fresh TLS 1.3 connections, `oha --disable-keepalive`)
measures about 177 handshakes/s at one connection and about 16/s at 8 or 64
connections, with connection errors (29 of 1000 in the smoke). The rate
falls with concurrency instead of rising, which points at serialization or
a stall in the handshake path (a shared lock, a blocking primitive on the
reactor thread, or an accept-side backlog), not at the cryptography.

## 2. Use cases
- **2.1** As a server, when 64 clients handshake at once, then the handshake
  rate is at least the single-connection rate and no connection errors occur.
- **2.2** As the test suite, then a test completes 1,000 concurrent TLS
  handshakes with zero failures and asserts the aggregate rate against the
  single-connection rate.

## 3. Acceptance
The bench row runs at 64 connections with zero errors and the rate scales.
