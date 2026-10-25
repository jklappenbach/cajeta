# Toolchain

Compile time, start-up, binary size, the build tool, the notebook kernel and the debugger, against the toolchains of five languages.

| Page | |
|---|---|
| [Compile time](Compile.md) | How long the compiler takes: the tour cold and warm, a synthetic 10k-line program generated in six languages from one description, and the link-time-optimization modes |
| [Start-up](Startup.md) | Time from exec to the first line of output for hello-world, as a native executable and through the JIT, cold and warm |
| [Binary size](BinarySize.md) | Size on disk of hello-world and of the tour, stripped |
| [Build tool](BuildTool.md) | The cost of cajeta build when nothing changed |
| [Notebook kernel start-up](NotebookKernel.md) | Time for the Jupyter kernel to answer its first kernel_info request and its first execution |
| [Debugger](Debugger.md) | Time to a first breakpoint under the debug adapter. cajeta dap has no attach request on v0.46.3, so the Cajeta row launches the tour against a warm JIT cache and stops at one breakpoint, while the competitors attach to a running process |

## Standing

Written after the first published run.
