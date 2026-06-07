# `cajeta.net` golden vector corpus (NET-13.2)

Checked-in, byte-exact test corpora shared across the networking
phases. This is the **data** half of the TDD strategy: every parser
and codec in the `cajeta.net` stack (and the crypto/codec primitives
that block it) pins against the same canonical vectors, so a
conformance regression in any phase surfaces as a deterministic,
reviewable diff against a known-answer file.

These vectors are deliberately **standalone** — they have no build
dependency on the parsers that consume them (which land in later line
items). Each downstream item wires its implementation to the relevant
corpus when it lands:

| Corpus | Source standard | Consumed by |
|---|---|---|
| `crypto/sha256.vectors` | FIPS 180-4 | NET-11.1 (`cajeta.hash.Sha256`) |
| `crypto/sha1.vectors` | FIPS 180-4 | NET-11.2 (`cajeta.hash.Sha1`) |
| `crypto/base64.vectors` | RFC 4648 | NET-11.3 (`cajeta.codec.Base64`) |
| `uri/parse.vectors` | RFC 3986 sec 3 | NET-6.1 (`Uri.parse`) |
| `uri/rfc3986-resolution.vectors` | RFC 3986 sec 5.4 | NET-6.4 (`Uri.resolve`) |
| `http/*.http` + `http/manifest.vectors` | RFC 7230 | NET-7.3 (incremental parser) |
| `http/chunked.vectors` | RFC 7230 sec 4.1 | NET-7.4 (chunked `BodyReader`) |
| `http/abuse.vectors` | RFC 7230 hardening | NET-7.3 (limit enforcement) |
| `ws/accept-key.vectors` | RFC 6455 sec 1.3 | NET-10.1 / NET-10.2 (handshake) |
| `ws/frames.vectors` | RFC 6455 sec 5.7 | NET-10.3 (frame codec) |

## File formats

Vector files are line-oriented, `#`-comment-aware, `|`-field-separated
text (UTF-8, LF newlines), **except** the `http/*.http` files, which
are byte-exact wire messages with **CRLF** line endings preserved on
disk (the parser is fed them verbatim). Shared token conventions
across the `|`-separated files:

- `<EMPTY>` — a present-but-empty field (e.g. an empty query string,
  an empty payload).
- `<NONE>` — an absent component (URI parse only; distinct from
  `<EMPTY>`).
- input-spec grammar (`crypto/*`): `empty`, `ascii:<text>`,
  `hex:<hexbytes>`, `repeat:<byte>:<n>`.

The `GoldenVectors.h` loader (in `test/net/`) parses every format into
structured records; `GoldenVectorsTests.cpp` is the self-validation
meta-test that asserts the whole corpus loads and is internally
consistent. Downstream phases `#include "net/GoldenVectors.h"` and
drive their implementation against the loaded records.

## Provenance

Every digest / encoded value in `crypto/*` and `ws/accept-key.vectors`
was cross-checked against the language-independent reference
implementations (Python `hashlib` / `base64`, which themselves track
the FIPS/RFC known-answer tables) before check-in. The `http/*.http`
Content-Length values and the `chunked.vectors` decoded outputs were
verified by an independent decode pass. If a `crypto` vector ever
fails after an implementation lands, the *implementation* is wrong —
the vector is the authority.
