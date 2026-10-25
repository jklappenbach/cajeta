# SipHash

`cajeta.hash.SipHash` — SipHash-2-4, the keyed hash a map uses when the keys are hostile.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `hash/siphash` | SipHash-2-4 of 32-byte keys, 1e7 times | reference C, Rust `siphasher`, Go `dchest/siphash`, Java, Python `siphash24` | digest |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: hash/siphash
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench siphash               # Cajeta
./bench.sh --bench siphash --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [SipHash](../../docs/stdlib/hash/SipHash.md)
- Source: `runtime/src/cajeta/hash/SipHash.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/hash/`
