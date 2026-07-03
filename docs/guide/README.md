# The Cajeta Guide

A linear walk through the toolchain, the language, and the standard library.
Chapters link to [tour](../../samples/tour/) demos you can run, and to the
[stdlib reference](../stdlib/README.md) for full API surfaces.

## Part I — Getting started

| Chapter | |
|---|---|
| [00 Introduction](00-introduction.md) | What Cajeta is; how the docs fit together |
| [01 Installation](01-installation.md) | cvm or source; verify the install |
| [02 Kick the tires](02-kick-the-tires.md) | The `cajeta` command surface |
| [03 Your first project](03-your-first-project.md) | init, build, the manifest, every project type |
| [04 Running](04-running.md) | Direct execution, run tasks, capabilities |
| [05 Debugging](05-debugging.md) | gdb, the DAP server, IntelliJ, VS Code |

## Part II — The language

| Chapter | |
|---|---|
| [06 Keywords](06-keywords.md) | The reserved-word table, from the lexer |
| [07 Comments](07-comments.md) | Line, block, and markdown doc comments |
| [08 Native types](08-native-types.md) | Integers, floats, microfloats, char, literals |
| [09 Type kinds](09-type-kinds.md) | class, interface, enum, view, annotation, @Kernel |
| [10 Allocation](10-allocation.md) | stack and heap |
| [11 Ownership](11-ownership.md) | Borrows, `#` transfers, drops |
| [12 Control flow](12-control-flow.md) | if, loops, switch statement + expression |
| [13 Strings](13-strings.md) | String, StringBuilder, `{}` formatting |
| [14 Templates](14-templates.md) | Monomorphization, bounds, wildcards |
| [15 Lambdas](15-lambdas.md) | Inference, borrow and transfer captures |
| [16 Operators](16-operators.md) | Overloading, derived forms, @AutoHash |
| [17 Inheritance](17-inheritance.md) | Single, multiple, interfaces |
| [18 Annotations](18-annotations.md) | Synthesis family; declaring your own |
| [19 DI & aspects](19-di-aspects.md) | @Component, @Inject, @Factory, advice |
| [20 Error handling](20-error-handling.md) | Exceptions; why there's no try-with-resources |
| [21 Reflection](21-reflection.md) | cajeta.reflect and the keep-set |

## Part III — The standard library

One chapter per package area, mirroring the tour. *(Planned.)*

## Part IV — Specialized

GPU kernels, graphics, embedded targets, toolchain deep-dives. *(Planned.)*
