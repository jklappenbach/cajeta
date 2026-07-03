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

Chapters 06–21: keywords, comments, native types, type kinds, allocation,
ownership, control flow, strings, templates, lambdas, operators, inheritance,
annotations, DI & aspects, error handling, reflection. *(In progress —
draft material in [drafts/](drafts/LanguageGuide.md).)*

## Part III — The standard library

One chapter per package area, mirroring the tour. *(Planned.)*

## Part IV — Specialized

GPU kernels, graphics, embedded targets, toolchain deep-dives. *(Planned.)*
