# Optimization: what is decided when, and what is remembered

A kernel written against one part has to find its shape on a part nobody
has measured, without a table of devices and without a vendor branch. This
page lays the mechanism out by **when** each decision is taken: at compile
time by the adaptor, at bind by the feasibility filter, at first use and at
load by measurement, and across runs by the store. The [map](README.md)
gives the model; this page gives the schedule. The specifications are
`xpu-kernel-adaptor`, `xpu-tile-manifest`, `xpu-tile-shape-selection` and
the research record `hardware-profile-tuning-findings`.

## 1. Compile time: the adaptor

The adaptor reconciles what a kernel declares with what the compiler
measures about its code, per backend, and writes the result into the
kernel's manifest.

**What the author declares** is only what nobody else can know:
`@Occupancy(maxThreads = N)` or `maxWaves = N`, the structural launch
ceiling; `@Wave(width = N)` where the body's indexing assumes a width;
`@FastMath`; `@Instantiate` and `@Requires` on a class template, the shapes
to build and the constraint a shape must satisfy; `@Unlowered` and
`@Unbounded`, each naming the plan item that tracks the condition.
Everything else is derived, because derived beats declared.

**What the compiler does with it**, per backend:

- scans every launch site for a constant block and **pins** the largest, so
  registers are budgeted for the real block rather than a worst-case 1024
  threads (without this, AMDGPU assumes the part's ceiling and NVPTX applies
  no bound; `@Occupancy` did nothing on NVPTX until cajeta `a171d180`,
  because the bound never reached ptxas, and every shape decision before it
  rested on an allocation that happened to fit);
- picks a **tier** per operand group for every cooperative-matrix verb
  (native `wmma` where the silicon has the shape; a portable software tile,
  distributed across the wave, elsewhere), demoting a straddling group whole
  rather than splitting it, and reports each distinct choice once;
- builds each `@Instantiate` member, refusing by name any that fails
  `@Requires`;
- emits the **manifest**: identity (name, target, a hash of the device code,
  the compiler), footprint from the code object (registers, spill bytes,
  static shared bytes, the pinned block, resident groups, feasible blocks),
  the access mode of every buffer parameter, and the instruction and tier of
  every native operation. Absent is a value: the CPU backend reports no
  register count, and a rule that needs one prunes nothing there;
- runs the **gates**: a kernel that spills, a kernel with no device code for
  a declared backend, an unbounded launch, a CPU vectorization that exceeded
  its deadline, each fails the build by name unless a declaration tracks it,
  and a declaration whose cause is gone is itself a failure (STALE).

Why the compile-time model is not the whole answer: the compiler's own
device model (`DeviceProfile`) knows two architectures by table, `gfx1151`
and `gfx1100`; on an RTX 4090 `lookupArch("sm_89")` is not found, so the
manifest's `feasibleBlocks` and `occupancyLimiter` are absent there. That is
by design the signal to read the driver at run time rather than extend the
table: supporting a device nobody has seen must cost no per-device source.
llama.cpp is the worked example of the alternative, nine per-architecture
configuration headers holding 2,027 cases over 22 quantization types, each
refitted by hand per part.

## 2. Bind: the feasibility filter

A **family** is a list of candidate shapes of one kernel design (a tile
template's members). At a problem `(M, N, K)` the family builds a
`ShapeFilter` over `DeviceFacts.current()`, read from the driver, and each
candidate's `KernelFacts.measured`, read from the manifest plus the launch's
dynamic shared request:

| rule | prunes when |
|---|---|
| R1 | the kernel spills |
| R2 | threads × registers exceed the register file |
| R3 | shared bytes exceed the per-block limit, opt-in included |
| R4 | no workgroup is resident on a multiprocessor |
| R5 | never; records whether the grid fills the device, or the split-K that would |
| R6 | never; records the padding the shape wastes at this problem |

Every pruned candidate is kept with its rule, its need and the limit; an
empty survivor set is a refusal that names them all, thrown as
`XpuLaunchException`, never a silent fallback. The filter is why the same
tile template serves an RTX 4090 and a Radeon 8060S from one source: the
128×128 member opts into the 4090's 99 KB ceiling and survives; on gfx1151
the block limit is 64 KB, R3 prunes it, and the 64×64 member serves. Nothing
in the library knows that about either part; the driver said it.

## 3. First use and load: the measured choice

**Among survivors.** `ShapeChoice` takes one survivor outright. With
several it first looks in the store under `shape.<family>.<M>x<N>x<K>`
against a build id made from the device UUID, the driver version and every
survivor's code hash. On a miss it times every survivor through the family's
probe at the `KernelTimer` tier: five interleaved rounds, so a drifting
clock cannot favour an arm; the fastest median wins; a lead under three
percent is a tie and recorded as such. The winner and its margin are
remembered, unless another process held a quarter or more of the device
while it was timed, in which case the result is used for this run and not
stored. A sweep can force a member (`setShapeOverride`), and a test can hold
the trial.

**Probes that run once at load**, each remembering its answer:

| probe | decides | how |
|---|---|---|
| the tile prewarm | the member for each prefill shape the engine will meet | the measured choice above, ahead of the first real prefill |
| the Cols probe, per format and batch width | whether the batched-decode kernel beats single-row launches | each candidate checked against the host mat-vec, then timed |
| the fused-Cols A/B | whether fused q\|k\|v and gate\|up launches beat separate ones | an in-place A/B on the real batched step |
| the mat-vec variant probe | prefetching, plain or two-row per weight shape | twenty-launch brackets, interleaved rounds, medians |
| `matrixCoresPay`, `tileBeatsDeq` | whether the matrix cores pay at all here; the tile against the int8 widen route | once per process, in memory only |

Some families time **in place**: the mixture-of-experts GEMM families have
no synthetic probe, so each layer between two marks is one sample of one
survivor, bracketed by `KernelTimer` on the device clock, until every
survivor has its rounds. The load-time warm-up holds the trial so that the
first prefill a user sees runs the untimed choice, and a measurement of such
a program is read only after the store has settled; a 30B prefill timed
before that read three times the kernel time.

**Per launch, without timing**: padding to the tile grain, the split-K count
for a problem that does not fill the device, one Cols launch or several
single-row launches, the batched-decode admission; all from the problem and
the remembered answers.

## 4. Across runs: the store

`Autotune` is one small file per (device key, name) under a directory the
application supplies: no parser, nothing to corrupt, a stale entry removed
by deleting a file. The **device key** is the backend plus the geometry the
answer depends on (multiprocessors × SIMDs, wave, registers, shared memory,
threads), so a store copied between machines is ignored rather than
believed, and a part that reports no geometry is keyed `unknown` and shares
nothing. `rememberFor` and `recallFor` carry a build id; a value recalled
against a different build is dropped and reported. A box with no writable
store still tunes once per execution. The test suites run with the store
off, so no earlier run's verdict reaches a test, and tests pin the probes'
answers where a route is asserted.

Keys the LLM engine's store holds today:

```
shape.q4kTcTile.<M>x<N>x<K>      shape.q5kTcTile…   shape.q6kTcTile…
shape.q4kWmmaMw.<M>x<N>x<K>      shape.q4kIdMw.<bucket>x<out>x<cols>
shape.q5kIdMw.<bucket>x<out>x<cols>        (each with a .margin twin)
colsPays-<type>-b<nc>            colsSeg-{qkv|gateup}-<rows>x<cols>-b<nc>
mvVariant-<type>-<rows>x<cols>-c<ncols>
partWgs
```

A separate **calibration store** holds device-wide measured facts (peaks,
bandwidth idle and under host load, bytes in flight, launch cost, event and
inter-queue cost), keyed by device, driver, compiler and power mode, served
from records built into the artifact and then the platform cache directory;
`cajeta calibrate` builds a snapshot for a fixed device.

## 5. The discipline

A device-timing bracket near the launch floor (6 to 13 µs a launch under
WSL2) reads up to twelve percent of noise, so a kernel-level choice uses
twenty-launch brackets, a warm round, interleaved rounds and medians, and a
gap is believed only after the generated code (PTX, then SASS) is diffed.
The event clock is calibrated against the host clock on first use (the CUDA
event clock runs 4.6 percent fast under WSL2) and a scale outside [0.5, 2]
is refused. Fresh device memory is poisoned in the test suites, so a kernel
reading what nothing wrote answers NaN deterministically. And a route change
in the inference library is gated by token-for-token agreement with the
reference implementation, because a route that is fast and wrong looks like
a win in every other instrument.

## 6. What is still design

Applying a run-time-chosen geometry to a kernel with a tunable block (the
adaptor's launch-geometry half) and self-tuning across runs beyond the
store's remember-and-recall are design under `xpu-kernel-adaptor` and the
hardware-profile research plan; today the kernels that adapt their geometry
do so as families through the filter and the measured choice above. The
scheduler's classification, admission and partition decisions are on the
[scheduler](scheduler.md) page with their status.
