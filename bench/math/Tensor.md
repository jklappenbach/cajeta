# Tensor

`cajeta.math.Tensor` — the keystone n-dimensional array. Element access is the row the tensor-element-access work targeted at 1.5 ns in release; the elementwise, broadcast, reduce and transpose rows are the operations every model is made of; matmul on the CPU is the kernel-pool path.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `math/tensor-element-get` | `flatGet` over every element of a 1M float64 tensor per trial, against a plain array and the `hostBuffer()` view; the three read paths must agree before a time is printed | NumPy item access (noted as interpreted), Eigen, `ndarray` | sum |
| `math/tensor-element-set` | `flatSet` over the same 1M elements | same | checksum |
| `math/tensor-elementwise` | `a * b + c` over 1e7 float32 | NumPy, Eigen, Rust `ndarray`, Go `gonum`, Java EJML | within epsilon |
| `math/tensor-broadcast` | add a 1e3 row to a 1e4 × 1e3 matrix | NumPy, Eigen, Rust `ndarray`, Go `gonum`, Java EJML | within epsilon |
| `math/tensor-reduce` | sum, max and mean along each axis of 1e4 × 1e3 | NumPy, Eigen, Rust `ndarray`, Go `gonum`, Java EJML | within epsilon |
| `math/tensor-transpose` | materialized transpose of 4096 × 4096 float32 | NumPy, Eigen, Rust `ndarray`, Go `gonum`, Java EJML | equality |
| `math/tensor-cast` | float32 → float64 → int32 over 1e7 | NumPy, Eigen, Rust `ndarray`, Go `gonum`, Java EJML | checksum |
| `math/matmul` | 1024 × 1024 float32 GEMM on the host | OpenBLAS through NumPy, Eigen, `ndarray` with BLAS, `gonum`, EJML | within epsilon |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: math/tensor-element-get
metric: time
```

```bench
id: math/tensor-element-set
metric: time
```

```bench
id: math/tensor-elementwise
metric: throughput
```

```bench
id: math/tensor-broadcast
metric: throughput
```

```bench
id: math/tensor-reduce
metric: throughput
```

```bench
id: math/tensor-transpose
metric: throughput
```

```bench
id: math/tensor-cast
metric: throughput
```

```bench
id: math/matmul
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench tensor-element-get               # Cajeta
./bench.sh --bench tensor-element-get --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Tensor](../../docs/stdlib/math/Tensor.md)
- Source: `runtime/src/cajeta/math/Tensor.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/math/`
