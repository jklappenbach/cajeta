# unsigned-int64-treated-as-signed — `>>` sign-propagates on uint64, and uint64 renders signed

## 1. Definition

Found 2026-08-08 implementing cajeta-cluster's consistent-hash ring,
where a 64-bit mixing function (murmur3's `fmix64`) silently produced
wrong values. Two shapes, one root: `uint64` is treated as signed by
operations that must be width- and sign-aware.

### Shape A — `>>` emits an ARITHMETIC shift for an unsigned operand

```cajeta
uint64 x = (uint64) 18446744073709551615;   // 2^64 - 1
System.stdout.println("" + (x >> 33));      // prints -1
                                            // expected 2147483647 (2^31 - 1)
```

The high bit is replicated instead of zero-filled, so every value with
bit 63 set shifts wrongly. This is the dangerous shape: it does not
crash, it produces a plausible number. In a hash mixer it destroys
avalanche — the ring built on it put 53% of keys on one of three nodes
and assigned all 32 partitions to a single node.

### Shape B — uint64 renders as signed in string conversion

```cajeta
uint64 h = Hash.of("a");                    // 12638187200555641996
System.stdout.println("of(a) = " + h);      // prints -5808556873153909620
```

The VALUE is correct — a `==` against the unsigned literal passes — but
every diagnostic, log line and test-failure message shows the wrapped
signed form, which is exactly when someone is trying to read a hash.

Bounding probes (all correct, so the defect is narrow):
- uint64 COMPARISONS are properly unsigned: `(uint64) -1 > (uint64) 1`
  is true, both as locals and as array elements.
- uint64 multiplication wraps correctly — the FNV-1a digests match the
  canonical vectors exactly.
- So arithmetic and ordering are fine; it is the shift's fill bit and
  the integer-to-string path that assume signed.

## 2. Requirements

- **2.1** `>>` on an unsigned operand is a LOGICAL shift (zero fill).
  If an arithmetic shift on unsigned is ever wanted it needs its own
  spelling; the default must follow the operand's signedness.
- **2.2** String conversion of a `uint64` renders the unsigned value.
- **2.3** Regression pins: `((uint64) -1) >> 33 == 2147483647`, and a
  uint64 above 2^63 round-trips through string conversion.

## 3. Workaround (in use)

cajeta-cluster's `Hash.shr33` masks the surviving bits after shifting:

```cajeta
static uint64 shr33(uint64 x) {
    return (x >> 33) & (uint64) 2147483647;   // keep 64 - 33 = 31 bits
}
```

`fmix64` calls it for all three of its shifts; with it, `Hash.position`
matches the standard murmur3 finalizer bit for bit (pinned in
`RingTest::hashIsThePinnedFnv1a64`) and the ring balances to
977/959/1064 over 3000 keys on three members. Delete the helper and
inline the shift once this is fixed.

## 4. Reproduction

The six-line program in §1 Shape A as a standalone `--emit=exe` run.
Shape B is the same program printing any uint64 above 2^63.
