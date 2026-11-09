# cajeta.hash

This section measures non-cryptographic hashing against the reference implementations. Cryptographic hashes moved to `dev.cajeta.crypto` and are measured there.

| Page | |
|---|---|
| [XXHash3](XXHash3.md) | The 64- and 128-bit XXH3 |
| [SipHash](SipHash.md) | SipHash-2-4, the keyed hash a map uses when the keys are hostile |
| [DefaultHasher](DefaultHasher.md) | The hasher behind String.hash, which a HashMap with string keys calls |
| [Hash](Hash.md) | The static hashing utilities |

## Standing

Geometric mean 2.1x behind over 8 benchmarks, no wins. XXH3-64 and XXH3-128 on 256 KiB inputs are at parity with the C library; short inputs and the struct and string hashers are 2x to 4x behind, not yet investigated.
