# Cryptography — Research Plan

> Initial research index for Cajeta. Status: initial pass (2026-05-28).

## Goals

Cajeta needs a comprehensive, modern cryptographic provider analogous to Bouncy Castle, but designed from the start around Cajeta's strengths: native LLVM 22 codegen, Rust-style borrow checking, and first-class GPU integration. The standard library must cover modern primitives — fast hashing (BLAKE3, SHA-3/Keccak, KangarooTwelve), AEAD (AES-GCM, AES-GCM-SIV, ChaCha20-Poly1305), KDF/password hashing (Argon2, scrypt, HKDF), the NIST post-quantum suite (ML-KEM, ML-DSA, SLH-DSA), and Curve25519-family ECC — anchored to authoritative specs (NIST FIPS, IETF RFCs, IACR papers) and reference implementations (libsodium, BoringSSL, RustCrypto, Bouncy Castle). A core constraint unique to a compiled language: Cajeta must give crypto authors a way to express and *preserve* constant-time execution through LLVM optimization passes, and to safely offload bulk symmetric work to the GPU.

## Research Index

### Hash functions and XOFs

- **What:** General-purpose and high-throughput cryptographic hashing, including extendable-output functions (XOFs) used as building blocks for KDFs, signatures, and PQC.
- **Why for Cajeta:** Hashing underpins almost everything else (HKDF, signatures, Merkle trees, content addressing). BLAKE3's binary-tree structure makes it the natural showcase for Cajeta's SIMD + multithreading + GPU ambitions. SHA-3/Keccak and its reduced-round derivatives are the substrate for the entire NIST PQC suite, so they must exist before PQC can be implemented.
- **Key papers / sources:**
  - [BLAKE3-specs (blake3.pdf): specifications, analysis, and design rationale](https://github.com/BLAKE3-team/BLAKE3-specs) — O'Connor, Aumasson, Neves, Wilcox-O'Hearn, 2020. Single-algorithm hash/XOF/KDF/MAC with a binary-tree mode enabling near-unbounded parallelism; compression function derived from BLAKE2s reduced to 7 rounds.
  - [The BLAKE3 Hashing Framework (draft-aumasson-blake3-00)](https://www.ietf.org/archive/id/draft-aumasson-blake3-00.html) — Aumasson et al., IETF I-D. Internet-Draft formalizing BLAKE3 for protocol use.
  - [FIPS 202: SHA-3 Standard — Permutation-Based Hash and Extendable-Output Functions](https://csrc.nist.gov/pubs/fips/202/final) — NIST, August 2015. Defines Keccak-p, the sponge construction, SHA3-224/256/384/512, and SHAKE128/256 XOFs. ([PDF](https://nvlpubs.nist.gov/nistpubs/fips/nist.fips.202.pdf))
  - [KangarooTwelve: fast hashing based on Keccak-p](https://eprint.iacr.org/2016/770.pdf) — Bertoni, Daemen, Hoffert, Peeters, Van Assche, Van Keer, IACR ePrint 2016/770. Tree-based, SIMD-parallel XOF using 12-round Keccak-p — roughly 2x faster than SHA-3 with a comfortable safety margin.
  - [RFC 9861: KangarooTwelve and TurboSHAKE](https://datatracker.ietf.org/doc/draft-irtf-cfrg-kangarootwelve) — CFRG. Standardizes K12 and the TurboSHAKE family (12-round Keccak-p XOFs) for IETF use.
  - [Keccak Team specifications](https://keccak.team/specifications.html) — Keccak/Xoodoo team. Authoritative reference for Keccak, KangarooTwelve, Ketje/Keyak, and related permutations.
- **Algorithms to capture:** BLAKE3 (hash/keyed-hash/derive-key/XOF), SHA3-224/256/384/512, SHAKE128, SHAKE256, cSHAKE, KMAC, TupleHash, ParallelHash, TurboSHAKE128/256, KangarooTwelve. Keep SHA-256/SHA-512 (FIPS 180-4) for compatibility.
- **Implementation notes:** Expose a single `Hasher` interface with streaming `update`/`finalize` plus an XOF `squeeze(n)`. BLAKE3 and K12 tree modes map cleanly onto Cajeta's task/SIMD model — chunk hashing is embarrassingly parallel and a strong first GPU-offload candidate (offload only above a length threshold to amortize transfer cost). The Keccak-f[1600] permutation should be a single `@simd`/intrinsic-backed function reused by SHA-3, SHAKE, K12, ML-KEM, ML-DSA, and SLH-DSA. Borrow checker: hasher state is owned and mutated through `&mut`; finalized digests are immutable owned buffers.

### AEAD (authenticated encryption with associated data)

- **What:** Symmetric encryption that simultaneously provides confidentiality and integrity over ciphertext + associated data.
- **Why for Cajeta:** This is the default symmetric API most application code touches (TLS records, file/disk encryption, tokens). Bulk AES-GCM and ChaCha20 throughput is exactly where GPU offload and AES-NI/CLMUL intrinsics pay off.
- **Key papers / sources:**
  - [RFC 8439: ChaCha20 and Poly1305 for IETF Protocols](https://www.rfc-editor.org/rfc/rfc8439.html) — Y. Nir, A. Langley, June 2018. Defines ChaCha20, Poly1305, and the combined AEAD_CHACHA20_POLY1305 (256-bit key, 96-bit nonce); obsoletes RFC 7539.
  - [RFC 8452: AES-GCM-SIV: Nonce Misuse-Resistant Authenticated Encryption](https://www.rfc-editor.org/info/rfc8452) — S. Gueron, A. Langley, Y. Lindell, April 2019. Nonce-misuse-resistant variant that degrades gracefully on nonce reuse.
  - [C2SP: chacha20-poly1305-siv](https://github.com/C2SP/C2SP/blob/main/chacha20-poly1305-siv.md) — Community Cryptography Specification Project. Misuse-resistant ChaCha20-Poly1305 variant spec.
  - [NIST SP 800-38D: Recommendation for Block Cipher Modes of Operation: Galois/Counter Mode (GCM) and GMAC](https://csrc.nist.gov/pubs/sp/800/38/d/final) — M. Dworkin, NIST, November 2007. The authoritative GCM/GMAC specification. ([PDF](https://nvlpubs.nist.gov/nistpubs/legacy/sp/nistspecialpublication800-38d.pdf))
- **Algorithms to capture:** AES-128/256-GCM, AES-GCM-SIV, ChaCha20-Poly1305, XChaCha20-Poly1305 (extended nonce), AES-SIV, and AES-CTR/CBC primitives underneath. Poly1305 and GHASH as MAC components.
- **Implementation notes:** API should make nonce management hard to get wrong — prefer a `seal`/`open` interface that takes a typed `Nonce` and refuses silent reuse; offer a misuse-resistant default (AES-GCM-SIV / XChaCha20-Poly1305). GHASH wants CLMUL/PMULL intrinsics; AES wants AES-NI. ChaCha20 is pure ARX, ideal for portable SIMD and GPU kernels. Keys should be zeroize-on-drop owned types enforced by the borrow checker; associated-data and plaintext borrows are read-only `&[u8]`.

### Key derivation and password hashing

- **What:** Deriving keys from shared secrets (HKDF) and from low-entropy passwords using memory-hard functions (Argon2, scrypt).
- **Why for Cajeta:** Needed for TLS key schedules, password storage, and PQC/ECDH post-processing. Memory-hardness is a deliberately anti-GPU/anti-ASIC property — interesting tension with Cajeta's GPU story (these specifically must *not* be cheaply offloadable).
- **Key papers / sources:**
  - [RFC 9106: Argon2 Memory-Hard Function for Password Hashing and Proof-of-Work](https://www.rfc-editor.org/rfc/rfc9106.html) — Biryukov, Dinu, Khovratovich, Josefsson, September 2021. Defines Argon2d/i/id; recommends Argon2id as the default.
  - [RFC 7914: The scrypt Password-Based Key Derivation Function](https://datatracker.ietf.org/doc/html/rfc7914.html) — Percival, Josefsson, 2016. Memory-hard PBKDF with tunable (N, r, p) cost.
  - [RFC 5869: HMAC-based Extract-and-Expand Key Derivation Function (HKDF)](https://www.rfc-editor.org/rfc/rfc5869.html) — H. Krawczyk, P. Eronen, May 2010. Extract-then-expand KDF used throughout TLS 1.3.
  - [cryptography.io — Key derivation functions](https://cryptography.io/en/latest/hazmat/primitives/key-derivation-functions/) — pyca/cryptography docs. Practical API-shape reference for KDF parameterization.
- **Algorithms to capture:** Argon2id/Argon2i/Argon2d, scrypt, HKDF (extract/expand), PBKDF2, HMAC. bcrypt for legacy interop.
- **Implementation notes:** Argon2/scrypt need large mutable scratch buffers — model as an owned arena sized from the cost parameter, freed deterministically (RAII via borrow checker, no GC pause). Deliberately keep these CPU/RAM-bound and do *not* GPU-offload (defeats the security property). HKDF/HMAC reuse the hash interface above. Provide a PHC-string encode/decode for Argon2 to match Bouncy Castle/libsodium interop.

### Post-quantum cryptography (NIST suite)

- **What:** Quantum-resistant KEM and signature schemes now standardized by NIST.
- **Why for Cajeta:** A next-generation language shipping in/after 2026 cannot omit PQC; hybrid (classical + PQC) handshakes are becoming the deployment norm. These are large, polynomial-arithmetic-heavy algorithms that benefit from SIMD/NTT and exercise Cajeta's numeric codegen.
- **Key papers / sources:**
  - [FIPS 203: Module-Lattice-Based Key-Encapsulation Mechanism Standard (ML-KEM)](https://csrc.nist.gov/pubs/fips/203/final) — NIST, August 13, 2024. Standardizes ML-KEM (from CRYSTALS-Kyber).
  - [FIPS 204: Module-Lattice-Based Digital Signature Standard (ML-DSA)](https://csrc.nist.gov/pubs/fips/204/final) — NIST, August 2024. Standardizes ML-DSA (from CRYSTALS-Dilithium).
  - [FIPS 205: Stateless Hash-Based Digital Signature Standard (SLH-DSA)](https://csrc.nist.gov/pubs/fips/205/final) — NIST, August 2024. Standardizes SLH-DSA (from SPHINCS+); conservative hash-based backup signature.
  - [Federal Register: Announcing FIPS 203, 204, 205](https://www.federalregister.gov/documents/2024/08/14/2024-17956/announcing-issuance-of-federal-information-processing-standards-fips-fips-203-module-lattice-based) — NIST/DoC, August 14, 2024. Official issuance notice.
  - [A Survey of Post-Quantum Cryptography Support in Cryptographic Libraries](https://arxiv.org/html/2508.16078v1) — arXiv 2508.16078, 2025. Compares PQC coverage across OpenSSL, BoringSSL, Bouncy Castle, wolfSSL, Botan, etc. — useful gap map for what Cajeta should prioritize.
  - [Benchmarking NIST-Standardised ML-KEM and ML-DSA on ARM Cortex-M0+: Performance, Memory, and Energy on the RP2040](https://arxiv.org/abs/2603.19340) — Rojin Chhetri, arXiv 2603.19340, March 2026. Embedded performance/memory/energy numbers (first isolated algorithm-level benchmarks on Cortex-M0+/RP2040), relevant to Cajeta on Jetson/ARM targets.
- **Algorithms to capture:** ML-KEM-512/768/1024, ML-DSA-44/65/87, SLH-DSA (SHA2/SHAKE parameter sets). Also track FN-DSA/Falcon (forthcoming FIPS 206) and HQC (NIST 4th-round KEM selection) for the backlog.
- **Implementation notes:** All three lean on Keccak (SHA-3/SHAKE) for sampling/hashing — reuse the permutation primitive from the hashing section. Number-theoretic transform (NTT) for ML-KEM/ML-DSA is a prime SIMD target and a candidate for GPU batch operations (e.g., server-side bulk handshakes). Rejection sampling in ML-DSA is inherently variable-time per-attempt but must not leak secret coefficients — needs constant-time selection primitives. Large keys/signatures (SLH-DSA sigs are kilobytes) argue for streaming/borrowed output buffers rather than return-by-value.

### Elliptic-curve cryptography (Curve25519 family)

- **What:** Modern ECC for key agreement (X25519) and signatures (Ed25519), plus the higher-security Curve448 variants.
- **Why for Cajeta:** Curve25519 is the de facto modern default (TLS, SSH, Signal, age, WireGuard) and pairs with ML-KEM/ML-DSA in hybrid handshakes. libsodium's design — Curve25519-only, no RSA — is a strong model for a clean default-secure API.
- **Key papers / sources:**
  - [RFC 7748: Elliptic Curves for Security](https://www.rfc-editor.org/rfc/rfc7748.html) — Langley, Hamburg, Turner, January 2016. Defines Curve25519/Curve448 and the X25519/X448 Diffie-Hellman functions.
  - [RFC 8032: Edwards-Curve Digital Signature Algorithm (EdDSA)](https://datatracker.ietf.org/doc/html/rfc8032) — Josefsson, Liusvaara, 2017. Defines Ed25519 and Ed448 signatures.
  - [Curve25519: New Diffie-Hellman Speed Records](https://iacr.org/archive/pkc2006/39580209/39580209.pdf) — Daniel J. Bernstein, PKC 2006 (LNCS 3958, pp. 207-228). The original Curve25519 paper introducing the high-speed, high-security X25519 Montgomery-curve DH function.
- **Algorithms to capture:** X25519, X448, Ed25519, Ed448, plus NIST P-256/P-384/P-521 (ECDSA/ECDH) for compliance interop. RSA-OAEP/PSS for legacy.
- **Implementation notes:** Field arithmetic over 2^255-19 must be constant-time — the canonical place to apply the LLVM constant-time intrinsics described below. The Montgomery ladder (X25519) and scalar clamping should be library-internal, exposing only `generate`/`agree`/`sign`/`verify`. Scalars and private keys are zeroize-on-drop owned types; public keys are plain 32-byte values. These are latency-bound, not throughput-bound — keep on CPU; GPU offload only matters for server-side batch verification.

### Constant-time guarantees, codegen, and the provider architecture

- **What:** Cross-cutting concern — ensuring the compiler does not destroy hand-written constant-time code, and structuring the library as a swappable provider (Bouncy-Castle-style) with codecs/encodings.
- **Why for Cajeta:** This is the single most Cajeta-specific risk. As a fresh LLVM 22 frontend, Cajeta can adopt or mirror the new LLVM constant-time intrinsics natively rather than fighting the optimizer with inline asm — a genuine differentiator over C/C++ crypto libraries.
- **Key papers / sources:**
  - [Introducing constant-time support for LLVM to protect cryptographic code](https://blog.trailofbits.com/2025/12/02/introducing-constant-time-support-for-llvm-to-protect-cryptographic-code/) — Trail of Bits, December 2025. Adds `__builtin_ct_select` family to LLVM 21+; lowers to `cmov` (x86-64), `csel` (ARM64), masked/bitwise fallbacks; an optimizer barrier preserving constant-timeness.
  - [CT-LLVM: Automatic Large-Scale Constant-Time Analysis](https://eprint.iacr.org/2025/338.pdf) — Zhang et al., IACR ePrint 2025/338. Automated constant-time verification over (optimized) LLVM IR.
  - [Towards Efficient Verification of Constant-Time Cryptographic Implementations](https://arxiv.org/pdf/2402.13506) — arXiv 2402.13506, 2024. CT-Prover: scalable constant-time verification; validated on ~19.7k LOC of C/C++ crypto.
  - [Robust Constant-Time Cryptography](https://arxiv.org/pdf/2311.05831) — arXiv 2311.05831, 2023. Methods for keeping constant-time properties robust across compiler transformations.
  - [Verifiable side-channel security of cryptographic implementations](https://eprint.iacr.org/2015/1241.pdf) — IACR ePrint 2015/1241. Foundational verified-CT work (e.g., constant-time curve/AES implementations).
  - [Comparison of cryptography libraries](https://en.wikipedia.org/wiki/Comparison_of_cryptography_libraries) — Wikipedia. Feature/algorithm matrix across OpenSSL, BoringSSL, libsodium, Bouncy Castle, Botan, RustCrypto — scope reference for "Bouncy-Castle-equivalent" coverage.
  - [Bouncy Castle](https://www.bouncycastle.org/) and [RustCrypto crates](https://lib.rs/cryptography) — provider-architecture and crate-decomposition models to emulate.
- **Algorithms to capture:** N/A (infrastructure) — but capture: constant-time select/swap primitives, secret-zeroizing types, RNG/CSPRNG interface (getrandom/`/dev/urandom`/RDRAND), and codecs: Base64 (std + URL-safe), Base32, Base16/hex, PEM, DER/ASN.1, Bech32, multibase.
- **Implementation notes:** Expose a `@constant_time` attribute (or `ct.select`/`ct.swap` builtins) in Cajeta that lowers to the LLVM ct intrinsics — make this a first-class language feature, not a library hack. Secret types should integrate with the borrow checker: a `Secret<T>` that is non-Copy, zeroized on drop, and that the type system can flag if it flows into a branch/index. Architect as a pluggable provider registry (algorithm name -> implementation) like JCE/Bouncy Castle, allowing FIPS-validated and GPU-accelerated providers to be swapped. RNG must be a clearly-owned, fail-closed source.

## PDF / paper backlog

- [x] BLAKE3 specification (blake3.pdf) — https://github.com/BLAKE3-team/BLAKE3-specs — papers/blake3-2020-spec.pdf
- [x] FIPS 202 (SHA-3) — https://nvlpubs.nist.gov/nistpubs/fips/nist.fips.202.pdf — papers/nist-2015-fips202-sha3.pdf
- [x] KangarooTwelve: fast hashing based on Keccak-p — https://eprint.iacr.org/2016/770.pdf — papers/bertoni-2016-kangarootwelve.pdf
- [ ] RFC 9861 (KangarooTwelve / TurboSHAKE) — https://datatracker.ietf.org/doc/draft-irtf-cfrg-kangarootwelve — (html-only datatracker landing page, not downloaded)
- [ ] RFC 8439 (ChaCha20-Poly1305) — https://www.rfc-editor.org/rfc/rfc8439.html — (html-only, no PDF rendition at rfc-editor, not downloaded)
- [ ] RFC 8452 (AES-GCM-SIV) — https://www.rfc-editor.org/info/rfc8452/ — (html-only, no PDF rendition at rfc-editor, not downloaded)
- [x] NIST SP 800-38D (AES-GCM) — https://nvlpubs.nist.gov/nistpubs/legacy/sp/nistspecialpublication800-38d.pdf — papers/nist-2007-sp800-38d-gcm.pdf
- [x] RFC 9106 (Argon2) — https://www.rfc-editor.org/rfc/rfc9106.pdf — papers/rfc-9106-argon2.pdf
- [ ] RFC 7914 (scrypt) — https://datatracker.ietf.org/doc/html/rfc7914.html — (html-only, no PDF rendition at rfc-editor, not downloaded)
- [ ] RFC 5869 (HKDF) — https://www.rfc-editor.org/rfc/rfc5869.html — (html-only, no PDF rendition at rfc-editor, not downloaded)
- [x] FIPS 203 (ML-KEM) — https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.203.pdf — papers/nist-2024-fips203-ml-kem.pdf
- [x] FIPS 204 (ML-DSA) — https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.204.pdf — papers/nist-2024-fips204-ml-dsa.pdf
- [x] FIPS 205 (SLH-DSA) — https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.205.pdf — papers/nist-2024-fips205-slh-dsa.pdf
- [x] PQC library support survey — https://arxiv.org/pdf/2508.16078 — papers/pqc-library-survey-2508.16078.pdf
- [ ] RFC 7748 (X25519 / Curve25519) — https://www.rfc-editor.org/rfc/rfc7748.html — (html-only, no PDF rendition at rfc-editor, not downloaded)
- [ ] RFC 8032 (EdDSA / Ed25519) — https://datatracker.ietf.org/doc/html/rfc8032 — (html-only, no PDF rendition at rfc-editor, not downloaded)
- [ ] Trail of Bits — LLVM constant-time support — https://blog.trailofbits.com/2025/12/02/introducing-constant-time-support-for-llvm-to-protect-cryptographic-code/ — (html-only blog post, not downloaded)
- [x] CT-LLVM — https://eprint.iacr.org/2025/338.pdf — papers/ct-llvm-2025-338.pdf
- [x] Towards Efficient Verification of Constant-Time Implementations — https://arxiv.org/pdf/2402.13506 — papers/ct-prover-2024-13506.pdf
- [x] Robust Constant-Time Cryptography — https://arxiv.org/pdf/2311.05831 — papers/robust-constant-time-2023-05831.pdf
- [x] Verifiable side-channel security of cryptographic implementations — https://eprint.iacr.org/2015/1241.pdf — papers/verifiable-side-channel-2015-1241.pdf
- [x] Curve25519: New Diffie-Hellman Speed Records (Bernstein, PKC 2006) — https://iacr.org/archive/pkc2006/39580209/39580209.pdf — papers/bernstein-2006-curve25519.pdf

## Open questions

- Should Cajeta expose constant-timeness as a language-level attribute (`@constant_time`, `ct.select`/`ct.swap` builtins) lowering directly to the LLVM 21+ ct intrinsics, and can the borrow checker statically forbid a `Secret<T>` from reaching a branch condition or array index?
- Which primitives should be GPU-offloadable by default? BLAKE3/K12 tree hashing and bulk AES-GCM/ChaCha20 are natural fits; Argon2/scrypt must explicitly *not* be (memory-hardness is the point). Where is the length/batch threshold that makes host<->device transfer worthwhile?
- Provider architecture: a JCE/Bouncy-Castle-style swappable registry vs. RustCrypto-style trait-based decomposition — which maps better onto Cajeta's type system and module model?
- FIPS posture: does Cajeta want a FIPS-validatable mode (approved algorithms only, self-tests, defined module boundary), and how does that interact with newer non-FIPS primitives like BLAKE3/Argon2?
- Hybrid handshakes: standardize on X25519+ML-KEM-768 (and Ed25519+ML-DSA) combinators as the default, mirroring current TLS deployment?
- RNG strategy: how to expose a fail-closed CSPRNG across Linux/Jetson/Windows/GPU contexts, and whether to seed from RDRAND/RDSEED, getrandom(2), or a userspace DRBG (SP 800-90A).
- Exact authors/venue for the previously-unverified items have been confirmed this pass: RFC 8452 (Gueron/Langley/Lindell, April 2019), NIST SP 800-38D (Dworkin, November 2007), the Bernstein Curve25519 paper (PKC 2006, LNCS 3958), and arXiv 2603.19340 (Chhetri, March 2026).
- ASN.1/DER and PEM handling: build a borrow-checked zero-copy parser, or wrap an existing one? Affects X.509, PKCS#8, and PQC key encoding.
