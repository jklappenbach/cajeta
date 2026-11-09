<section class="hero home-hero">
  <p class="kicker">The Cajeta programming language</p>
  <h1>Familiar syntax.<br /><em>Borrowed</em> discipline.</h1>
  <p class="hero-lede">
    Cajeta is a compiled systems language in the C++/Java family with
    Rust-style ownership. It has explicit <code>stack</code>/<code>heap</code>
    allocation, a static borrow checker, and no garbage collector, so the
    safety is settled at compile time and the runtime just runs. Furthermore,
    it is designed around AI agents from the ground up. The compiler is an
    MCP server, and every library ships the guidance an agent needs to use
    it.
  </p>
  <!-- Site-absolute, not `../guide/README.md`. These anchors are RAW HTML, and
       rehype-docs-links only rewrites `a` elements in the parsed hast tree —
       a raw block never becomes one, so a `.md` href here ships verbatim and
       404s. Markdown links elsewhere in the docs are rewritten and stay `.md`. -->
  <div class="hero-ctas">
    <a class="button primary" href="/guide/">Read the guide</a>
    <a class="button ghost" href="/guide/01-installation/">Install with cvm</a>
    <a class="button ghost" href="/stdlib/">Browse the stdlib</a>
  </div>
</section>

Cajeta compiles through LLVM, either as an optimizing, caching JIT for
portable archives or as ahead-of-time native binaries. It pairs the
ergonomics of Java (classes, interfaces, packages, annotations) with the
control of C++ and the memory discipline of Rust. Every heap value has one
owner, borrows are the default, and ownership transfers with a single `#`
operator, all checked before the program ever runs. It specializes where
predictable performance and safety have to coexist. That means services and
CLIs, embedded targets, and, through a first-class compute path, GPU kernels
written in the same language as the host program.

<div class="pillar-grid">
  <div class="pillar">
    <h3>Safe without a GC</h3>
    <p>Borrow checking happens at compile time, drops are deterministic, and there are no pauses. Use-after-move is caught as a compiler error.</p>
  </div>
  <div class="pillar">
    <h3>Syntax you already know</h3>
    <p>If you read Java or C++, you read Cajeta. It has classes, monomorphized templates in the C++ style, operator overloading, and annotations.</p>
  </div>
  <div class="pillar">
    <h3>GPU as a language feature</h3>
    <p>Kernels, cooperative matrices, and device buffers are part of the language and the stdlib.</p>
  </div>
  <div class="pillar">
    <h3>Batteries included</h3>
    <p>The stdlib covers collections, streams, fibers and channels, JSON/CSV codecs, networking, and reflection, with one doc per class.</p>
  </div>
</div>

## Built for agents

Most Cajeta from here on will be written with an agent in the loop. What
limits an agent on an unfamiliar language is access to authoritative,
specific guidance at the moment it writes the line. So Cajeta ships that
guidance in the toolchain, next to the code it describes, served over a
protocol the agent already speaks.

<div class="feature-grid">
  <div class="feature">
    <h3>The compiler is an MCP server</h3>
    <p><code>cajeta compiler-mcp</code> speaks Model Context Protocol over stdio, with no second binary, no daemon, and no network. It exposes <code>searchSkills</code>, <code>listSkills</code>, and <code>getSkills</code>, and its <code>initialize</code> instructions tell the agent to look guidance up before it writes code.</p>
  </div>
  <div class="feature">
    <h3>Skills, not scraped docs</h3>
    <p>Skills are hand-written guides keyed to a library, package, class, or method. They are written against the failure modes that make a Java-fluent model crash. More than 180 ship embedded in the compiler, covering the language, the toolchain, and every stdlib package.</p>
  </div>
  <div class="feature">
    <h3>Every library ships its skills</h3>
    <p>Skills are part of the <code>.cja</code> archive format. Put them in <code>skills/*.md</code> and the build validates, indexes, and packages them beside the bitcode. Resolve a dependency and you have its guidance offline, pinned to the version you resolved.</p>
  </div>
  <div class="feature">
    <h3>Search that survives a typo</h3>
    <p>Matching is fuzzy and hierarchical. A misspelled name still resolves, and one query returns the symbol, its neighbours, and the overview above it. Every result is a stable <code>cja-skill://</code> URI, which is a valid cache key and is identical on every machine.</p>
  </div>
</div>

The same three operations are available to humans and CI as
`cajeta search-skill` / `list-skills` / `get-skills`, from the same in-process
cores. See [the built-in MCP server](../specification/mcp/CompilerMcp.md) and
[the skill system](../specification/mcp/Skills.md).

## From IR to silicon

Cajeta is built on LLVM. Source lowers to LLVM's intermediate
representation (IR), is optimized, including auto-vectorization onto the
host's SIMD registers, and is converted directly into machine code for
the processors that will execute it.

<div class="feature-grid">
  <div class="feature">
    <h3>Write once, run anywhere</h3>
    <p>Executables and libraries ship as IR in compressed <code>.cja</code> archives. The optimized JIT lowers them for whatever CPU they land on and caches the machine code. Warm starts skip compilation entirely, so execution is fast from the first call.</p>
  </div>
  <div class="feature">
    <h3>Native binaries</h3>
    <p>The same IR compiles ahead of time into a conventional executable for a specific target, for deployments that want to forego the JIT machinery altogether.</p>
  </div>
  <div class="feature">
    <h3>Every GPU, one source</h3>
    <p>Kernels lower through LLVM directly to GPU architectures (NVIDIA PTX, AMD GCN, and SPIR-V for the Vulkan-portable path) and to the host CPU as a vectorised target. A kernel declares what only its author knows, and the compiler measures the rest into a manifest. At run time the device's own facts filter the candidate shapes, and a timed choice, remembered per device, picks among the survivors. Read <a href="/xpu/">how it adapts, with every kernel in the LLM and XGBoost libraries</a>, and see <a href="/xpu/llm/">the language-model engine built on it</a>.</p>
  </div>
  <div class="feature">
    <h3>Lazy linking</h3>
    <p>The runtime and standard library link as bitcode, and only what your program references is materialized into the output, which keeps binaries and JIT working sets small.</p>
  </div>
</div>

## The toolchain

<div class="feature-grid">
  <div class="feature">
    <h3>cajeta — the builder</h3>
    <p>One binary covers the whole loop, from project init and dependency resolution through build, test, lint, docs, packaging, and publish. Tasks live in <code>cajeta.json</code>, and there is no fixed lifecycle to fight.</p>
  </div>
  <div class="feature">
    <h3>cvm — version management</h3>
    <p>cvm installs and switches toolchains the way rustup does for Rust, with <code>cvm install latest</code>, <code>cvm default</code>, and <code>cvm doctor</code>. Toolchains live under <code>~/.cajeta/versions</code>.</p>
  </div>
  <div class="feature">
    <h3>IDE plugins</h3>
    <p>A JetBrains/IntelliJ IDEA plugin ships with build tooling built in. IDE support is IntelliJ-family today, with VS Code next. Language Server and Debug Adapter protocols give any LSP/DAP-capable editor completion and breakpoints.</p>
  </div>
  <div class="feature">
    <h3>Olla — the public repository</h3>
    <p>Olla is the package registry for Cajeta libraries, a growing set spanning machine learning, network analysis, gradient boosting, HTTP, and logging. Publishes are signed, artifacts are content-addressed, and the logs are transparent. Browse and publish at <a href="https://olla.cajeta.dev">olla.cajeta.dev</a>.</p>
  </div>
</div>

## Platform-independent libraries

Three library families make Cajeta a working platform for numerical and
visual computing. They are written once and run across vendors and devices.

<div class="feature-grid lib-grid">
  <div class="feature">
    <h3>Nucleo <span class="tag">data science &amp; ML</span></h3>
    <p>Nucleo is the consolidated core for porting the Python scientific stack. It offers tensors and Arrow-native dataframes over one buffer model, lazy expression fusion, autograd, and numpy/scipy/torch-style surfaces, with kernel-level matrix and tensor math spanning SIMD CPU and GPU.</p>
  </div>
  <div class="feature">
    <h3>XPU <span class="tag">compute</span></h3>
    <p>XPU is portable device compute. It provides kernels, pipelined GEMM primitives, cooperative matrices, and device profiles that target NVIDIA, AMD, and Vulkan-class hardware from one source.</p>
  </div>
  <div class="feature">
    <h3>GFX <span class="tag">graphics</span></h3>
    <p>GFX provides graphics primitives built on XPU, including streaming geometry, ray queries, and samplers, for visualization and rendering without committing to a vendor API.</p>
  </div>
</div>

## Where to go next

- **New to Cajeta?** Start with the [guide](../guide/README.md), a linear
  walk from installation to reflection in 22 short chapters.
- **Looking up an API?** The [stdlib reference](../stdlib/README.md) has one
  document per public class.
- **Wiring up an agent?** [CompilerMcp](../specification/mcp/CompilerMcp.md) has
  the server and its tools. [Skills](../specification/mcp/Skills.md) covers the
  format, the authoring levels, and how to ship skills with your own library.
- **Designing against the language?** The [language specification](../specification/01-introduction.md)
  holds the deep-dive documents behind every subsystem.
- **Wondering about speed?** The [benchmarks](../../bench/README.md) are laid
  out like the stdlib reference, with one page per class, each naming what is
  measured, against which language or system, and how it is checked. On the
  first published run Cajeta is at parity with Rust and C++ on loops, calls,
  I/O and GPU kernels, and behind, often by a lot, on small-object allocation,
  locks, strings, streams and the cpu tensor paths; every page says which.
