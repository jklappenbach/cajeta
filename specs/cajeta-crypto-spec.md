# cajeta-crypto — cryptography leaves the stdlib

Status: draft, 2026-10-17. Decisions with Julian the same day are in §1.5.
Plan: `agents/cajeta-crypto-plan.md`.

## 1. Definition

### 1.1 Purpose

Cryptography changes on a different clock from the language. Algorithms are
deprecated, providers change what they sign with, OpenSSL ships fixes. In the
stdlib, every one of those changes waits on a compiler release. A library,
`dev.cajeta.crypto` in a new `cajeta-crypto` repo, ships when the change is
ready and consumers move by changing one pin.

This spec moves everything cryptographic out of the stdlib into that library,
and adds the one capability auth needs now: public-key signatures for JWTs, so
that cajeta-cloud can verify tokens from a real identity provider and its
memory driver can issue the same kind of token.

### 1.2 What moves, what stays

| Today | Moves to | Notes |
|---|---|---|
| `cajeta.hash.Sha1`, `Sha256`, `MD5`, `Blake3` | `dev.cajeta.crypto` | Still implement `cajeta.hash.Hasher` |
| `cajeta.hash.HmacSha1`, `HmacSha256` | `dev.cajeta.crypto` | |
| `cajeta.hash.Pbkdf2`, `Totp` | `dev.cajeta.crypto` | |
| `cajeta.hash.SecureRandom` | `dev.cajeta.crypto` | Backed by OpenSSL's `RAND_bytes` |
| `cajeta.io.net.tls.*` (5 types) and `runtime/native/cajeta_tls.c` | `dev.cajeta.crypto.tls` | The C engine moves into the library's native archive |
| — | `dev.cajeta.crypto.sig` | New: RS256 and ES256 (§4) |

Stays in the stdlib: `cajeta.hash.Hash`, `Hasher`, `DefaultHasher`,
`SipHash`, `XXHash3` (hash tables and fingerprints, not cryptography),
`cajeta.codec.Base32`, `Base64`, JSON, and `cajeta.io.net` sockets.

### 1.3 Why no dependency cycle exists (measured 2026-10-17)

- Nothing in the stdlib outside `cajeta.hash` uses the nine types, and
  nothing outside `cajeta.io.net.tls` uses TLS. The other hits are comments.
- Inside `cajeta.hash` the crypto types use only each other and the
  `Hasher` interface, which stays.
- The library depends on the stdlib alone. Its consumers are cajeta-http
  (`Sha1` for the WebSocket accept key, `Sha256` for ETags and download
  checksums, TLS in 10 files), cajeta-cloud (`HmacSha256`, `Pbkdf2`,
  `SecureRandom`, `Sha256`, `Totp`) and cajeta-primavera (`SecureRandom`,
  `Totp`). None of them is a dependency of the library.
- The compiler's C++ build tool uses OpenSSL directly (`Lockfile.h`), not the
  cajeta types.

### 1.4 Scope

- The move of §1.2, with tests, docs, skills and the tour.
- The new signature package (§4).
- The library's native archive, built per platform (§5).
- Release order and the stdlib removal (§6).
- cajeta-cloud verifying RS256 and ES256, and its memory driver issuing ES256
  (§7). cajeta-http and cajeta-primavera switching their imports.

### 1.5 Decisions (Julian, 2026-10-17)

- **1.5.1** A separate library, not the stdlib: "I'm hesitant to put yet one
  more thing in stdlib. Especially something like encryption, which will have
  frequent changes."
- **1.5.2** Existing C libraries do the work through `@Native`; porting
  algorithms to cajeta comes later.
- **1.5.3** All nine `cajeta.hash` crypto types move, not only the keyed ones.
- **1.5.4** They are removed from the stdlib in one release, no deprecation
  cycle.
- **1.5.5** Signatures: RS256 and ES256 verification, plus ES256 signing so the
  memory driver issues real asymmetric tokens.
- **1.5.6** TLS moves too.
- **1.5.7** Only what is needed now. A comprehensive library (AEAD, key
  agreement, post-quantum, a constant-time `secret` type, the June
  `agents/cajeta/Crypto-plan.md`) is later work.

### 1.6 Non-goals

- PS256, EdDSA, RSA signing, RSA key generation, PEM and DER key files, X.509
  beyond what TLS already does.
- Encryption of data at rest, AEAD, key agreement.
- Porting any algorithm to pure cajeta. The moved hashes stay the cajeta code
  they are today; the new signatures and TLS call OpenSSL.
- Zeroizing private keys on drop. The ES256 private scalar lives in an owned
  `int8[]` and is documented as such.
- Windows `cajeta test` for the library in CI (library CI has no Windows leg).
  Windows is checked by hand on Phoenix (§5.4).

### 1.7 Systems

OpenSSL 3 (`libcrypto`, `libssl`), already on every toolchain's link line.
The `native-libraries` subsystem and its per-platform `.a` baking, as
cajeta-codec uses it for zlib. `cajeta.codec.Base64` (url-safe),
`cajeta.codec.json`. `dev.cajeta.unit` for tests. Olla trusted publishing.

---

## 2. Feature: the library and its repo

- **2.1** `jklappenbach/cajeta-crypto` is a public GitHub repo with the
  standard layout: `cajeta.json`, `src/main/cajeta/dev/cajeta/crypto/...`,
  tests, `native/`, `docs/`, a tour sample, `README.md`, Apache-2.0.
- **2.2** A push to main builds the native archives, builds the library and
  runs its tests. A `v*` tag on `release` also publishes a GitHub Release and
  publishes to Olla through trusted publishing.
- **2.3** A consumer adds `"dev.cajeta.crypto": "0.1.0"` and nothing else. The
  `.cja` carries the native archive for every supported platform, so no
  consumer sets `CAJETA_NATIVE_PATH`.
- **2.4** A program that calls nothing from the library links no native code
  from it.

## 3. Feature: the moved types

- **3.1** Each moved type keeps its name, its public methods and its
  behaviour. A consumer migrates by changing `import cajeta.hash.Sha256` to
  `import dev.cajeta.crypto.Sha256` and nothing else. The TLS types keep their
  names under `dev.cajeta.crypto.tls`.
- **3.2** The tests that cover them today come with them, so coverage does not
  drop. That includes the compiler's `Sha1Tests`, `Sha256Tests`,
  `CryptoPrimitivesTests`, `OtpPrimitivesTests`, `TlsClientTests`,
  `TlsConnectionTests`, `TlsEngineTests` and `TlsLoopbackTest`, rewritten as
  library tests where they test the library.
  A compiler test that only borrows one of these types as an example keeps
  testing the compiler with another stdlib type instead.
- **3.3** `SecureRandom.fill` and `bytes` draw from OpenSSL's `RAND_bytes`, and
  a failure raises rather than returning short or predictable bytes.
- **3.4** While cajeta v0.43.x still carries its own copies, a program may use
  both the stdlib's and the library's types in one build. The library's C
  symbols are named `__cajeta_crypto_*`, never `__cajeta_tls_*`, so the two
  never define one symbol twice.
- **3.5** The docs move with the types: `docs/stdlib/hash/{Sha1,Sha256,MD5,
  Blake3,HmacSha1,HmacSha256,Pbkdf2,Totp,SecureRandom}.md`, the TLS docs, the
  hash and TLS skills, the tour's `HashDemo` and `tls` samples, and the
  `samples/profile` hash benchmarks.

## 4. Feature: signatures (`dev.cajeta.crypto.sig`)

- **4.1** When a consumer has an RSA public key as a JWK (`kty` RSA, `n`, `e`),
  it builds an `RsaPublicKey` from it. A modulus under 2048 bits is refused
  with `InvalidKeyException` (RFC 7518 §3.3).
- **4.2** When a consumer has a P-256 public key as a JWK (`kty` EC, `crv`
  P-256, `x`, `y`), it builds an `EcPublicKey`. A point not on the curve, or
  any other curve, is refused with `InvalidKeyException`.
- **4.3** `Signatures.verify(alg, key, signingInput, signature)` answers true
  or false for RS256 (RSASSA-PKCS1-v1_5 with SHA-256) and ES256 (ECDSA P-256
  with SHA-256, the signature as the 64-byte `r || s` of RFC 7518 §3.4). A
  malformed signature, a wrong length, or a key whose type does not match the
  algorithm answers false and never raises. The algorithm comes from the
  caller, never from the key, so a token cannot choose its own check.
- **4.4** When the JWK carries `alg` and it disagrees with the algorithm being
  checked, verification answers false.
- **4.5** `EcKeyPair.generateP256()` makes a fresh key pair from OpenSSL's
  generator. `sign(signingInput)` returns the 64-byte `r || s`.
  `publicJwk(kid)` returns the public half as a JWK JSON object with `kty`,
  `crv`, `x`, `y`, `kid`, `alg` ES256 and `use` sig, and never `d`.
- **4.6** A signature made by `sign` verifies through 4.3 with the key from
  `publicJwk`, and fails when any byte of the input or the signature changes.
- **4.7** The published test vectors pass: RFC 7515 Appendix A.2 (RS256) and
  A.3 (ES256), with their JWKs from the RFC.

## 5. Feature: the native archive

- **5.1** One static archive, `libcajeta_crypto.a`, holds the TLS engine and
  the signature and random shims. It is built from the repo's C sources
  against OpenSSL 3 headers on each platform's own runner, and baked into the
  `.cja` for linux-x64, linux-arm64, macos-arm64 and windows-x64, the
  toolchain's four release targets.
- **5.2** The archive links against the `-lssl -lcrypto` the compiler already
  puts on every link line. Nothing in the archive vendors OpenSSL.
- **5.3** `cajeta test` of the library and of its consumers runs the native
  code in the JIT on Linux, where OpenSSL's symbols come from the `cajeta`
  process.
- **5.4** On Windows, `cajeta.exe` links OpenSSL statically. Whether the JIT
  can resolve the archive's OpenSSL calls is measured on Phoenix before the TLS
  move (plan Unit 0). If it cannot, the JIT exports the OpenSSL entry points
  the archive calls, by the same bridge-table contract the `__cajeta_tls_*`
  symbols use today, and `scripts/check-jit-win-symbols.sh` checks them.

## 6. Feature: release order and the stdlib removal

- **6.1** dev.cajeta.crypto 0.1.0 publishes first, built on cajeta v0.43.1,
  which still carries its own copies.
- **6.2** Then cajeta-http 0.9.0, cajeta-cloud 0.4.0 and cajeta-primavera
  0.4.0 publish, importing only from the library.
- **6.3** Then one cajeta release removes all nine types, `cajeta.io.net.tls`,
  `cajeta_tls.c`, the TLS object embedded in the compiler for `--emit=exe`,
  the `__cajeta_tls_*` bridge entries, the runtime's
  `__cajeta_secure_random_fill`, and the moved tests, docs and samples. Code
  importing a removed type fails to compile with the type's name in the
  error; the release notes give the new import.
- **6.4** After that release, every library is rebuilt and tested on it.
  Primavera's CI and register-user sample pass.

## 7. Feature: cajeta-cloud uses it

- **7.1** `Jwt.decode` verifies RS256 and ES256 against the key in the
  verifier's JWKS whose `kid` matches the token's header, through §4.3.
  RS256 and ES256 are no longer refused by name.
- **7.2** HS256 is refused. A verifier that holds only public keys cannot
  check a symmetric signature, and accepting it would let a token signed with
  a public key as an HMAC secret pass.
- **7.3** The memory driver signs ES256 under a per-process `EcKeyPair` and
  serves its public JWK from `jwks()`, so tests exercise the same path a real
  provider's tokens take.
- **7.4** Primavera's `Security.md` limits section drops "HS256 only", and
  `primavera-spec.md` §8.2 says crypto primitives come from
  `dev.cajeta.crypto`.

## 8. Acceptance (spec level)

- **8.1** dev.cajeta.crypto 0.1.0 is on Olla with native archives for the four
  platforms.
- **8.2** Every test that covered a moved type before the move passes in the
  library after it.
- **8.3** The RFC 7515 vectors pass, and an ES256 token from the memory driver
  verifies through cajeta-cloud's verifier and through primavera's
  `WebSecurity`.
- **8.4** The cajeta release in 6.3 contains no `cajeta.hash` crypto type and
  no TLS. Its full test gate, regression gate and Windows release legs pass.
- **8.5** cajeta-http, cajeta-cloud and cajeta-primavera pass their suites on
  that release, primavera's CI included. cajeta-http's Autobahn workflow still
  matches 0.6.0.
