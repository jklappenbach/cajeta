# Kernel parity

`dev.cajeta.llm` — the parity legs: one matrix-multiply shape per row, device-timed on each side's own stream, joined under the five rules the parity harness enforces (same op, type and shape; the engine labelled with its build; the same residency; the device timer tier; a calibration row that reproduced a recorded number).

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `llm/kernel-parity` | device time per shape; variants per (op, type, shape) from `census/parity-<backend>.tsv` | the llama.cpp counterpart kernel named in `census/oracle-map.tsv` | values credited against the host or oracle |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: llm/kernel-parity
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench kernel-parity               # Cajeta
./bench.sh --bench kernel-parity --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/llm/`
- The catalog of kernels and counterparts: [catalog](../../docs/kernels/catalog-llm.md)
