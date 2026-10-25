# MeshSimplifier

`cajeta.xpu.mesh.MeshSimplifier` — mesh simplification on device.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `xpu/mesh-simplify` | simplify a 1e6-triangle mesh to 1e5 | meshoptimizer (CPU reference) | triangle count and Hausdorff bound |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: xpu/mesh-simplify
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench mesh-simplify               # Cajeta
./bench.sh --bench mesh-simplify --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [MeshSimplifier](../../../docs/stdlib/xpu/mesh/MeshSimplifier.md)
- Source: `runtime/src/cajeta/xpu/mesh/MeshSimplifier.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/xpu/`
