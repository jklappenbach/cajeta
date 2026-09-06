# HTML-only sources — engineering simulation & task-graph GPU runtimes

These exist only as web pages (vendor docs, blogs) or behind paywalls with no open PDF.
PDF-backed sources live in `papers/` with `.pdf.txt` markers.

---

## Getting Started with CUDA Graphs (NVIDIA developer blog)

- url: https://developer.nvidia.com/blog/cuda-graphs/

The primary numeric source for the launch-overhead argument. Three-step model: stream
capture (`cudaStreamBeginCapture` / `cudaStreamEndCapture`) or explicit node construction →
`cudaGraphInstantiate` (pre-initializes kernel descriptors) → `cudaGraphLaunch` (one
submission for the whole graph). Measured per-kernel cost for a chain of short kernels:

| scenario | time per kernel |
|---|---|
| individual launches, sync each kernel | 9.6 µs |
| overlapped launches, one sync per timestep | 3.8 µs |
| CUDA graph launch | 3.4 µs |
| kernel execution alone (floor) | 2.9 µs |

Graph creation + instantiation costs **~400 µs, once**, amortizing to ~0.02 µs per kernel
over 1000 iterations. The recommended pattern is exactly the simulation loop: build and
instantiate once on the first step, then `launch graph; wait` for every remaining step.
The stated precondition for benefit is that "the same graph executes multiple times", which
is why iterative/time-stepped codes are the canonical case.

---

## HIP graphs (AMD ROCm documentation)

- url: https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/hip_runtime_api/hipgraph.html

The AMD counterpart, API-compatible in shape: `hipStreamBeginCapture`/`hipStreamEndCapture`
for retrofitting existing stream code with minimal changes, or explicit construction with
`hipGraphAddKernelNode()` etc. for fine-grained control; then `hipGraphInstantiate()` →
`hipGraphExec_t` → `hipGraphLaunch()` onto a stream. Instantiation front-loads validation
and setup so it is not paid per launch. The stated motivating condition is the same one a
simulation inner loop hits: "a GPU kernel might run faster than the time it takes for the
framework to set up and launch the kernel". Benefit is qualitative in the docs (a speedup
curve for "launching many short-running kernels"), with the caveat that the setup cost only
pays back on iterative workloads.

---

## StarPU: a unified platform for task scheduling on heterogeneous multicore architectures
(Augonnet, Thibault, Namyst, Wacrenier — CCPE 23(2), 2011)

- url: https://onlinelibrary.wiley.com/doi/abs/10.1002/cpe.1631
- open-access attempt failed: the HAL record (https://inria.hal.science/inria-00550877) is
  behind an Anubis challenge and returns HTML, not the PDF.

The canonical task-based-runtime reference. A task is a *codelet* (several
per-architecture implementations of the same operation) plus a set of data handles with
access modes; the runtime builds the dependency graph from those handles, keeps a software
cache of each handle's replicas per memory node, issues transfers implicitly, and dispatches
to whichever worker (CPU core, GPU) its pluggable scheduling policy picks — including
performance-model-driven policies (HEFT/dmda) that predict per-worker execution time and
transfer time. Recorded here because the paper itself is paywalled; the substantially
overlapping open source in `papers/courtes-2013-starpu-extensions.pdf` covers the same task
model concretely.
