<section class="hero home-hero">
  <p class="kicker">The Cajeta programming language</p>
  <h1>Familiar syntax.<br /><em>Borrowed</em> discipline.</h1>
  <p class="hero-lede">
    Cajeta is a compiled systems language in the C++/Java family with
    Rust-style ownership: explicit <code>stack</code>/<code>heap</code>
    allocation, a static borrow checker, and no garbage collector — the
    safety is settled at compile time, the runtime just runs.
  </p>
  <div class="hero-ctas">
    <a class="button primary" href="../guide/README.md">Read the guide</a>
    <a class="button ghost" href="../guide/01-installation.md">Install with cvm</a>
    <a class="button ghost" href="../stdlib/README.md">Browse the stdlib</a>
  </div>
</section>

Cajeta compiles ahead of time to native code through LLVM, pairing the
ergonomics of Java — classes, interfaces, packages, annotations — with the
control of C++ and the memory discipline of Rust. One owner per heap value,
borrows by default, ownership transfer with a single `#` operator, all
checked before the program ever runs. It specializes where predictable
performance and safety have to coexist: services and CLIs, embedded targets,
and — through a first-class compute path — GPU kernels written in the same
language as the host program.

<div class="pillar-grid">
  <div class="pillar">
    <h3>Safe without a GC</h3>
    <p>Compile-time borrow checking, deterministic drops, no pauses. Use-after-move is a compiler error, not a crash report.</p>
  </div>
  <div class="pillar">
    <h3>Syntax you already know</h3>
    <p>If you read Java or C++, you read Cajeta. Classes, generics-style templates (monomorphized), operator overloading, annotations.</p>
  </div>
  <div class="pillar">
    <h3>GPU as a language feature</h3>
    <p>Kernels, cooperative matrices, and device buffers are part of the language and stdlib — not a bolted-on toolkit.</p>
  </div>
  <div class="pillar">
    <h3>Batteries included</h3>
    <p>Collections, streams, fibers and channels, JSON/CSV codecs, networking, reflection — a coherent stdlib, one doc per class.</p>
  </div>
</div>

## The toolchain

<div class="feature-grid">
  <div class="feature">
    <h3>cajeta — the builder</h3>
    <p>One binary for the whole loop: project init, dependency resolution, build, test, lint, docs, packaging, publish. Tasks live in <code>cajeta.json</code>; there is no fixed lifecycle to fight.</p>
  </div>
  <div class="feature">
    <h3>cvm — version management</h3>
    <p>Installs and switches toolchains the way rustup does for Rust: <code>cvm install latest</code>, <code>cvm default</code>, <code>cvm doctor</code>. Toolchains live under <code>~/.cajeta/versions</code>.</p>
  </div>
  <div class="feature">
    <h3>IDE plugins</h3>
    <p>A JetBrains/IntelliJ IDEA plugin with build tooling built in, plus Language Server and Debug Adapter protocols so any LSP/DAP-capable editor gets completion and breakpoints.</p>
  </div>
  <div class="feature">
    <h3>Olla — the public repository</h3>
    <p>The package registry for Cajeta libraries: signed publishes, content-addressed artifacts, transparent logs. Browse and publish at <a href="https://olla.cajeta.dev">olla.cajeta.dev</a>.</p>
  </div>
</div>

## Platform-independent libraries

Three library families make Cajeta a working platform for numerical and
visual computing — written once, running across vendors and devices.

<div class="feature-grid lib-grid">
  <div class="feature">
    <h3>Nucleo <span class="tag">data science &amp; ML</span></h3>
    <p>The consolidated core for porting the Python scientific stack: tensors and Arrow-native dataframes over one buffer model, lazy expression fusion, autograd, and numpy/scipy/torch-style surfaces.</p>
  </div>
  <div class="feature">
    <h3>XPU <span class="tag">compute</span></h3>
    <p>Portable device compute: kernels, pipelined GEMM primitives, cooperative matrices, and device profiles that target NVIDIA, AMD, and Vulkan-class hardware from one source.</p>
  </div>
  <div class="feature">
    <h3>GFX <span class="tag">graphics</span></h3>
    <p>Graphics primitives built on XPU — streaming geometry, ray queries, samplers — for visualization and rendering without committing to a vendor API.</p>
  </div>
</div>

## Where to go next

- **New to Cajeta?** Start with the [guide](../guide/README.md) — a linear
  walk from installation to reflection, in 22 short chapters.
- **Looking up an API?** The [stdlib reference](../stdlib/README.md) has one
  document per public class.
- **Designing against the language?** The [specification index](../specification/README.md)
  holds the deep-dive documents behind every subsystem.
- **Wondering about speed?** The [benchmarks](../../bench/README.md) compare
  Cajeta against incumbent runtimes on shared workloads.
