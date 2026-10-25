# MeshSimplifier

`cajeta.xpu.mesh.MeshSimplifier` — mesh simplification on device.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `xpu/mesh-simplify` | simplify 19,800 triangles to 1,980 on the host (`MeshSimplifier` is host code with a quadratic driver; the page's 1e6 → 1e5 shape is a skip row with that reason, and meshoptimizer runs both) | meshoptimizer (CPU reference) | triangle count and a one-sided vertex-to-surface Hausdorff bound |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

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
